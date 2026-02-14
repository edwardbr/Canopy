/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// Standard C++ headers
#include <algorithm>

// RPC headers
#include <rpc/rpc.h>

#ifdef _IN_ENCLAVE
#include <fmt/format-inl.h>
#else
#include <fmt/format.h>
#endif
namespace rpc
{
    ////////////////////////////////////////////////////////////////////////////
    // service

    thread_local service* current_service_ = nullptr;
    service* service::get_current_service()
    {
        return current_service_;
    }
    void service::set_current_service(service* svc)
    {
        current_service_ = svc;
    }

    std::atomic<uint64_t> service::zone_id_generator_ = 0;
    zone service::generate_new_zone_id()
    {
        auto count = ++zone_id_generator_;
        return {count};
    }

    object service::generate_new_object_id() const
    {
        auto count = ++object_id_generator_;
        return {count};
    }

#ifdef CANOPY_BUILD_COROUTINE
    service::service(const char* name, zone zone_id, const std::shared_ptr<coro::io_scheduler>& scheduler)
        : zone_id_(zone_id)
        , name_(name)
        , io_scheduler_(scheduler)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_service_creation(name, zone_id, destination_zone{0});
#endif
    }
    service::service(const char* name, zone zone_id, const std::shared_ptr<coro::io_scheduler>& scheduler, child_service_tag)
        : zone_id_(zone_id)
        , name_(name)
        , io_scheduler_(scheduler)
    {
        // No telemetry call for child services
    }

#else
    service::service(const char* name, zone zone_id)
        : zone_id_(zone_id)
        , name_(name)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_service_creation(name, zone_id, destination_zone{0});
#endif
    }
    service::service(const char* name, zone zone_id, child_service_tag)
        : zone_id_(zone_id)
        , name_(name)
    {
        // No telemetry call for child services
    }

#endif

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
            wrapped_object_to_stub_.clear();
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
        auto* addr = ptr->get_address();
        if (addr)
        {
            std::lock_guard g(stub_control_);
            auto item = wrapped_object_to_stub_.find(addr);
            if (item != wrapped_object_to_stub_.end())
            {
                auto obj = item->second.lock();
                if (obj)
                    return obj->get_id();
            }
        }
        else
        {
            return casting_interface::get_object_id(*ptr);
        }
        return {};
    }

    bool service::check_is_empty() const
    {
        std::lock_guard l(stub_control_);
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
        for (auto item : wrapped_object_to_stub_)
        {
            auto stub = item.second.lock();
            if (!stub)
            {
                RPC_WARNING("wrapped stub zone_id {}, wrapped_object has been released but not deregistered in the "
                            "service suspected unclean shutdown",
                    std::to_string(zone_id_));
            }
            else
            {
                RPC_WARNING("wrapped stub zone_id {}, wrapped_object {} has not been deregistered in the service "
                            "suspected unclean shutdown",
                    std::to_string(zone_id_),
                    std::to_string(stub->get_id()));
            }
            success = false;
        }

        for (auto item : service_proxies_)
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
                auto transport = svcproxy->get_transport();
                RPC_WARNING("service proxy zone_id {}, destination_zone_id {} "
                            "has not been released in the service suspected unclean shutdown",
                    std::to_string(zone_id_),
                    std::to_string(item.first));

                for (auto proxy : svcproxy->get_proxies())
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
        std::lock_guard g(service_proxy_control_);
        const child_service* child_svc = dynamic_cast<const child_service*>(this);
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

    CORO_TASK(int)
    service::send(uint64_t protocol_version,
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
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        // overriden versions of this functions my have more to do with these parameters
        std::ignore = tag;
        std::ignore = in_back_channel;
        std::ignore = out_back_channel;

#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_send(
                zone_id_, destination_zone_id, caller_zone_id, object_id, interface_id, method_id);
        }
#endif
        if (destination_zone_id != zone_id_.as_destination())
        {
            RPC_ASSERT(false); // this should be going to the pass through
            CO_RETURN error::TRANSPORT_ERROR();
        }
        current_service_tracker tracker(this);
        if (protocol_version < rpc::LOWEST_SUPPORTED_VERSION || protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
        {
            RPC_ERROR("Unsupported service version {} in send", protocol_version);
            CO_RETURN rpc::error::INVALID_VERSION();
        }

        std::weak_ptr<object_stub> weak_stub = get_object(object_id);
        auto stub = weak_stub.lock();
        if (stub == nullptr)
        {
            RPC_INFO("Object gone - stub has already been released");
            CO_RETURN rpc::error::OBJECT_GONE();
        }

        auto ret
            = CO_AWAIT stub->call(protocol_version, encoding, caller_zone_id, interface_id, method_id, in_data, out_buf_);

        CO_RETURN ret;
    }

    CORO_TASK(void)
    service::post(uint64_t protocol_version,
        encoding encoding,
        uint64_t tag,
        caller_zone caller_zone_id,
        destination_zone destination_zone_id,
        object object_id,
        interface_ordinal interface_id,
        method method_id,
        const rpc::span& in_data,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        // todo!
        std::ignore = encoding;
        std::ignore = caller_zone_id;
        std::ignore = destination_zone_id;
        std::ignore = object_id;
        std::ignore = interface_id;
        std::ignore = method_id;
        std::ignore = in_data;

        // overriden versions of this functions my have more to do with these parameters
        std::ignore = tag;
        std::ignore = in_back_channel;

#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_post(
                zone_id_, destination_zone_id, caller_zone_id, object_id, interface_id, method_id);
        }
#endif

        if (destination_zone_id != zone_id_.as_destination())
        {
            RPC_ASSERT(false); // this should be going to the pass through
            RPC_ERROR("Unsupported post destination");
            CO_RETURN;
        }

        current_service_tracker tracker(this);

        // Log that post was received
        RPC_INFO("service::post received for destination_zone={} object_id={}",
            destination_zone_id.get_val(),
            object_id.get_val());

        if (protocol_version < rpc::LOWEST_SUPPORTED_VERSION || protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
        {
            RPC_ERROR("Unsupported service version {} in post", protocol_version);
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
        std::vector<char> out_buf_dummy;
        CO_AWAIT stub->call(protocol_version, encoding, caller_zone_id, interface_id, method_id, in_data, out_buf_dummy);

        // Log that post was delivered to local stub
        RPC_INFO(
            "service::post delivered to local stub for object_id={} in zone={}", object_id.get_val(), zone_id_.get_val());

        CO_RETURN;
    }

    CORO_TASK(void)
    service::clean_up_on_failed_connection(const std::shared_ptr<rpc::service_proxy>& destination_zone,
        rpc::shared_ptr<rpc::casting_interface> input_interface)
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
    CORO_TASK(int)
    service::get_descriptor_from_interface_stub(caller_zone caller_zone_id,
        rpc::casting_interface* iface,
        std::function<std::shared_ptr<rpc::i_interface_stub>(std::shared_ptr<rpc::object_stub>)> fn,
        std::shared_ptr<rpc::object_stub>& stub,
        interface_descriptor& descriptor,
        bool optimistic)
    {

        auto* pointer = iface->get_address();
        // find the stub by its address
        {
            std::lock_guard g(stub_control_);
            auto item = wrapped_object_to_stub_.find(pointer);
            if (item != wrapped_object_to_stub_.end())
            {
                stub = item->second.lock();
                // Don't mask the race condition - if stub is null here, we have a serious problem
                RPC_ASSERT(stub != nullptr);
            }
            else
            {
                // else create a stub
                auto id = generate_new_object_id();
                stub = std::make_shared<object_stub>(id, shared_from_this(), pointer);
                std::shared_ptr<rpc::i_interface_stub> interface_stub = fn(stub);
                stub->add_interface(interface_stub);
                wrapped_object_to_stub_[pointer] = stub;
                stubs_[id] = stub;
                stub->on_added_to_zone(stub);
            }
        }
        auto ret = CO_AWAIT stub->add_ref(optimistic, false, caller_zone_id);
        if (ret != rpc::error::OK())
        {
            CO_RETURN ret;
        }
        descriptor = {stub->get_id(), zone_id_.as_destination()};
        CO_RETURN error::OK();
    }

    CORO_TASK(int)
    service::add_ref_local_or_remote_return_descriptor(uint64_t protocol_version,
        caller_zone caller_zone_id,
        rpc::casting_interface* iface,
        std::function<std::shared_ptr<rpc::i_interface_stub>(std::shared_ptr<rpc::object_stub>)> fn,
        std::shared_ptr<rpc::object_stub>& stub,
        interface_descriptor& descriptor,
        bool optimistic)
    {
        // This is ALWAYS an out parameter case
        if (caller_zone_id.is_set() && !iface->is_local())
        {
            // Inline prepare_out_param logic here for out parameter binding
            auto object_proxy = iface->get_object_proxy();
            RPC_ASSERT(object_proxy != nullptr);
            auto object_service_proxy = object_proxy->get_service_proxy();
            RPC_ASSERT(object_service_proxy->zone_id_ == zone_id_);
            auto destination_zone_id = object_service_proxy->get_destination_zone_id();
            auto object_transport = object_service_proxy->get_transport();
            auto object_id = object_proxy->get_object_id();

            RPC_ASSERT(caller_zone_id.is_set());
            RPC_ASSERT(destination_zone_id.is_set());

            std::vector<rpc::back_channel_entry> empty_in;
            std::vector<rpc::back_channel_entry> empty_out;
            int err_code = 0;
            std::shared_ptr<rpc::i_marshaller> marshaller;

            if (caller_zone_id == destination_zone_id.as_caller())
            {
                marshaller = object_transport;
                if (!marshaller)
                {
                    CO_RETURN error::TRANSPORT_ERROR();
                }
            }
            else
            {
                marshaller = object_transport->get_passthrough(destination_zone_id, caller_zone_id.as_destination());
                if (!marshaller)
                {
                    std::shared_ptr<rpc::transport> caller_transport;
                    {
                        std::lock_guard g(service_proxy_control_);
                        auto found = transports_.find(caller_zone_id.as_destination());
                        if (found == transports_.end())
                        {
                            RPC_ERROR("No service proxy found for caller zone {}", caller_zone_id.get_val());
                            CO_RETURN error::ZONE_NOT_FOUND();
                        }
                        else
                        {
                            caller_transport = found->second.lock();
                            if (!caller_transport)
                            {
                                RPC_ERROR("Failed to obtain valid caller service_proxy");
                                CO_RETURN error::SERVICE_PROXY_LOST_CONNECTION();
                            }
                        }
                    }

                    if (object_transport == caller_transport)
                    {
                        marshaller = object_transport;
                    }
                    else
                    {
                        marshaller = transport::create_pass_through(object_service_proxy->get_transport(),
                            caller_transport,
                            shared_from_this(),
                            destination_zone_id,
                            caller_zone_id.as_destination());
                    }
                }
            }

            // PROBLEM: Single known_direction used for BOTH build_destination_route AND build_caller_route
            // - For destination: should point toward object (e.g., zone 6)
            // - For caller: should point toward caller (e.g., zone 10)
            // Current workaround uses zone_id_ but causes loop
            auto known_direction = zone_id_.as_known_direction_zone();

            RPC_DEBUG("add_ref_local_or_remote_return_descriptor: zone={}, dest_zone={}, caller_zone={}, "
                      "known_direction={}, object_transport={}, obj_adj_zone={}",
                zone_id_.get_val(),
                destination_zone_id.get_val(),
                caller_zone_id.get_val(),
                known_direction.get_val(),
                object_transport != nullptr,
                object_transport ? object_transport->get_adjacent_zone_id().get_val() : 0);

            err_code = CO_AWAIT marshaller->add_ref(protocol_version,
                destination_zone_id,
                object_id,
                caller_zone_id,
                known_direction,
                rpc::add_ref_options::build_destination_route | rpc::add_ref_options::build_caller_route
                    | (optimistic ? add_ref_options::optimistic : add_ref_options::normal),
                empty_in,
                empty_out);
            if (err_code != rpc::error::OK())
            {
                RPC_ERROR("add_ref_local_or_remote_return_descriptor add_ref failed with code {}", err_code);
                CO_RETURN err_code;
            }
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                telemetry_service->on_service_proxy_add_ref(zone_id_,
                    destination_zone_id,
                    caller_zone_id,
                    object_id,
                    known_direction,
                    rpc::add_ref_options::build_destination_route | rpc::add_ref_options::build_caller_route
                        | (optimistic ? add_ref_options::optimistic : add_ref_options::normal));
            }
#endif

            descriptor = {object_id, destination_zone_id};
            CO_RETURN error::OK();
        }

        // For local interfaces or when caller_zone_id is not set, create a local stub
        auto* pointer = iface->get_address();
        {
            {
                std::lock_guard g(stub_control_);
                auto item = wrapped_object_to_stub_.find(pointer);
                if (item != wrapped_object_to_stub_.end())
                {
                    stub = item->second.lock();
                    RPC_ASSERT(stub != nullptr);
                }
                else
                {
                    auto id = generate_new_object_id();
                    stub = std::make_shared<object_stub>(id, shared_from_this(), pointer);
                    std::shared_ptr<rpc::i_interface_stub> interface_stub = fn(stub);
                    stub->add_interface(interface_stub);
                    wrapped_object_to_stub_[pointer] = stub;
                    stubs_[id] = stub;
                    stub->on_added_to_zone(stub);
                }
            }
            auto ret = CO_AWAIT stub->add_ref(optimistic, true, caller_zone_id); // outcall=true
            if (ret != rpc::error::OK())
            {
                CO_RETURN ret;
            }
        }
        descriptor = {stub->get_id(), zone_id_.as_destination()};
        CO_RETURN error::OK();
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
    CORO_TASK(int)
    service::try_cast(uint64_t protocol_version,
        caller_zone caller_zone_id,
        destination_zone destination_zone_id,
        object object_id,
        interface_ordinal interface_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        std::ignore = caller_zone_id;
        std::ignore = in_back_channel;
        std::ignore = out_back_channel;

        if (destination_zone_id != zone_id_.as_destination())
        {
            RPC_ASSERT(false); // this should be going to the pass through
            CO_RETURN error::TRANSPORT_ERROR();
        }
        current_service_tracker tracker(this);

        if (protocol_version < rpc::LOWEST_SUPPORTED_VERSION || protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
        {
            RPC_ERROR("Unsupported service version {} in try_cast", protocol_version);
            CO_RETURN rpc::error::INVALID_VERSION();
        }
        std::weak_ptr<object_stub> weak_stub = get_object(object_id);
        auto stub = weak_stub.lock();
        if (!stub)
        {
            RPC_ERROR("Invalid data - stub is null in try_cast");
            CO_RETURN error::INVALID_DATA();
        }
        CO_RETURN stub->try_cast(interface_id);
    }

    CORO_TASK(int)
    service::add_ref(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        known_direction_zone known_direction_zone_id,
        add_ref_options build_out_param_channel,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        bool optimistic = !!(build_out_param_channel & add_ref_options::optimistic);
        bool build_caller_channel = !!(build_out_param_channel & add_ref_options::build_caller_route);
        bool build_dest_channel = !!(build_out_param_channel & add_ref_options::build_destination_route)
                                  || build_out_param_channel == add_ref_options::normal
                                  || build_out_param_channel == add_ref_options::optimistic;

        RPC_ASSERT(!!build_caller_channel || !!build_dest_channel);

        current_service_tracker tracker(this);
        if (build_caller_channel)
        {
            if (zone_id_.as_caller() != caller_zone_id)
            {
                auto caller_transport = get_transport(caller_zone_id.as_destination());
                auto error = CO_AWAIT caller_transport->add_ref(protocol_version,
                    destination_zone_id,
                    object_id,
                    caller_zone_id,
                    zone_id_.as_known_direction_zone(),
                    add_ref_options::build_caller_route
                        | (optimistic ? add_ref_options::optimistic : add_ref_options::normal),
                    in_back_channel,
                    out_back_channel);
                if (error != rpc::error::OK())
                {
                    RPC_ERROR("Caller channel add_ref failed with code {}", error);
                    CO_RETURN error;
                }
            }
            else
            {
                std::lock_guard g(service_proxy_control_);
                auto destination_transport = inner_get_transport(destination_zone_id);
                if (destination_transport == nullptr)
                {
                    destination_transport = inner_get_transport(known_direction_zone_id.as_destination());
                    if (destination_transport == nullptr)
                    {
                        RPC_ERROR("Destination transport not found for zone {}", destination_zone_id.get_val());
                        CO_RETURN rpc::error::ZONE_NOT_FOUND();
                    }
                    inner_add_transport(destination_zone_id, destination_transport);
                }
            }
        }

        if (build_dest_channel)
        {
            if (zone_id_.as_destination() != destination_zone_id)
            {
                auto dest_transport = get_transport(destination_zone_id);
                CO_RETURN CO_AWAIT dest_transport->add_ref(protocol_version,
                    destination_zone_id,
                    object_id,
                    caller_zone_id,
                    zone_id_.as_known_direction_zone(),
                    build_out_param_channel & (~add_ref_options::build_caller_route),
                    in_back_channel,
                    out_back_channel);
            }

            // service has the implementation
            if (protocol_version < rpc::LOWEST_SUPPORTED_VERSION || protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
            {
                RPC_ERROR("Unsupported service version {} in add_ref", protocol_version);
                CO_RETURN rpc::error::INVALID_VERSION();
            }

            if (object_id == dummy_object_id)
            {
                CO_RETURN rpc::error::OK();
            }

            std::weak_ptr<object_stub> weak_stub = get_object(object_id);
            auto stub = weak_stub.lock();
            if (!stub)
            {
                RPC_ASSERT(false);
                CO_RETURN rpc::error::OBJECT_NOT_FOUND();
            }

            {
                std::lock_guard g(service_proxy_control_);
                auto dest_transport = inner_get_transport(caller_zone_id.as_destination());
                if (!dest_transport)
                {
                    dest_transport = inner_get_transport(known_direction_zone_id.as_destination());
                    inner_add_transport(caller_zone_id.as_destination(), dest_transport);
                }
            }

            auto ret = CO_AWAIT stub->add_ref(
                !!(build_out_param_channel & add_ref_options::optimistic), false, caller_zone_id);
            if (ret != rpc::error::OK())
            {
                CO_RETURN ret;
            }
        }
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_add_ref(
                zone_id_, destination_zone_id, object_id, caller_zone_id, known_direction_zone_id, build_out_param_channel);
        }
#endif
        CO_RETURN rpc::error::OK();
    }

    uint64_t service::release_local_stub(
        const std::shared_ptr<rpc::object_stub>& stub, bool is_optimistic, caller_zone caller_zone_id)
    {
        std::lock_guard l(stub_control_);
        uint64_t count = stub->release(is_optimistic, caller_zone_id);
        if (!is_optimistic && !count)
        {
            {
                stubs_.erase(stub->get_id());
            }
            {
                auto* pointer = stub->get_castable_interface()->get_address();
                auto it = wrapped_object_to_stub_.find(pointer);
                if (it != wrapped_object_to_stub_.end())
                {
                    wrapped_object_to_stub_.erase(it);
                }
                else
                {
                    // if you get here make sure that get_address is defined in the most derived class
                    RPC_ASSERT(false);
                }
            }
            stub->reset();
        }
        return count;
    }

    void service::cleanup_service_proxy(const std::shared_ptr<rpc::service_proxy>& other_zone)
    {
        std::ignore = other_zone;
    }

    CORO_TASK(int)
    service::release(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        release_options options,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel)
    {
        std::ignore = destination_zone_id;
        std::ignore = in_back_channel;
        std::ignore = out_back_channel;

        current_service_tracker tracker(this);

        if (protocol_version < rpc::LOWEST_SUPPORTED_VERSION || protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
        {
            RPC_ERROR("Unsupported service version {} in release", protocol_version);
            CO_RETURN rpc::error::INVALID_VERSION();
        }

        std::shared_ptr<rpc::object_stub> stub;
        uint64_t count = 0;
        // these scope brackets are needed as otherwise there will be a recursive lock on a mutex in rare cases when the stub is reset
        {
            {
                // a scoped lock
                std::lock_guard l(stub_control_);
                auto item = stubs_.find(object_id);
                if (item == stubs_.end())
                {
                    // Stub has been deleted - can happen with optimistic_ptr when shared_ptr is released
                    CO_RETURN rpc::error::OBJECT_NOT_FOUND();
                }

                stub = item->second.lock();
            }

            if (!stub)
            {
                RPC_ASSERT(false);
                CO_RETURN rpc::error::OBJECT_NOT_FOUND();
            }
            // this guy needs to live outside of the mutex or deadlocks may happen
            count = stub->release(!!(release_options::optimistic & options), caller_zone_id);
            if (!count && !(release_options::optimistic & options))
            {
                // When shared count drops to zero, notify all transports that have optimistic references
                // Get the optimistic reference map before releasing the stub
                std::vector<caller_zone> optimistic_refs;
                {
                    std::lock_guard lock(stub->references_mutex_);
                    optimistic_refs.reserve(stub->optimistic_references_.size());
                    for (const auto& [zone, count_atomic] : stub->optimistic_references_)
                    {
                        uint64_t count_val = count_atomic.load(std::memory_order_acquire);
                        if (count_val > 0)
                        {
                            optimistic_refs.push_back(zone);
                        }
                    }
                }

                {
                    // a scoped lock - erase stub from maps
                    std::lock_guard l(stub_control_);
                    {
                        stubs_.erase(object_id);
                    }
                    {
                        auto* pointer = stub->get_castable_interface()->get_address();
                        auto it = wrapped_object_to_stub_.find(pointer);
                        if (it != wrapped_object_to_stub_.end())
                        {
                            wrapped_object_to_stub_.erase(it);
                        }
                        else
                        {
                            RPC_ASSERT(false);
                            CO_RETURN rpc::error::OBJECT_NOT_FOUND();
                        }
                    }
                } // Release stub_control_ lock before calling object_released

                stub->reset();

                // Now notify all transports that had optimistic references
                // IMPORTANT: This must be done AFTER releasing stub_control_ mutex to avoid deadlock
                // In synchronous mode or with local transport, object_released can chain back
                // to code that tries to acquire stub_control_ again
                for (const auto& caller_zone_id : optimistic_refs)
                {
                    auto transport = get_transport(caller_zone_id.as_destination());
                    if (transport)
                    {
                        // Call object_released on the transport to notify the remote zone
                        // This is a fire-and-forget operation
                        CO_AWAIT transport->object_released(protocol_version,
                            zone_id_.as_destination(), // destination is where the object lives
                            object_id,
                            caller_zone_id,
                            {}); // empty back channel
                    }
                }
            }
        }

#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_release(zone_id_, destination_zone_id, object_id, caller_zone_id, options);
        }
#endif
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(void)
    service::object_released(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        std::ignore = in_back_channel;
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_object_released(zone_id_, destination_zone_id, caller_zone_id, object_id);
        }
#endif
        if (caller_zone_id != zone_id_.as_caller())
        {
            RPC_ASSERT(false); // this should be going to the pass through
            CO_RETURN;
        }
        current_service_tracker tracker(this);

        if (protocol_version < rpc::LOWEST_SUPPORTED_VERSION || protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
        {
            RPC_ERROR("Unsupported service version {} in object_released", protocol_version);
            CO_RETURN;
        }

        // Notify that an object has been released
        CO_AWAIT notify_object_gone_event(object_id, destination_zone_id);
    }

    CORO_TASK(void)
    service::transport_down(uint64_t protocol_version,
        destination_zone destination_zone_id,
        caller_zone caller_zone_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel)
    {
        std::ignore = in_back_channel;
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_transport_down(zone_id_, destination_zone_id, caller_zone_id);
        }
#endif

        if (destination_zone_id != zone_id_.as_destination())
        {
            RPC_ASSERT(false); // this should be going to the pass through
            CO_RETURN;
        }
        current_service_tracker tracker(this);

        if (protocol_version < rpc::LOWEST_SUPPORTED_VERSION || protocol_version > rpc::HIGHEST_SUPPORTED_VERSION)
        {
            RPC_ERROR("Unsupported service version {} in transport_down", protocol_version);
            CO_RETURN;
        }

        RPC_INFO("Transport down notification received from caller_zone={} to destination_zone={}",
            caller_zone_id.get_val(),
            destination_zone_id.get_val());

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
                caller_zone_id.get_val());

            // Release all references from the failed zone and trigger object_released events
            for (auto& [obj_id, stub] : stubs_to_cleanup)
            {
                bool should_delete = stub->release_all_from_zone(caller_zone_id);

                if (should_delete)
                {
                    // Shared count reached zero - stub should be deleted
                    RPC_INFO("transport_down: Object {} ref count dropped to zero, cleaning up", obj_id.get_val());

                    // Remove from maps
                    stubs_.erase(obj_id);
                    auto* pointer = stub->get_castable_interface()->get_address();
                    wrapped_object_to_stub_.erase(pointer);

                    // Track for notification
                    objects_to_notify.push_back(obj_id);

                    stub->reset();
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
        RPC_ASSERT(destination_zone_id != zone_id_.as_destination());
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
        RPC_ASSERT(service_proxy->get_destination_zone_id() != zone_id_.as_destination());
        std::lock_guard g(service_proxy_control_);
        inner_add_zone_proxy(service_proxy);
    }

    void service::inner_add_transport(destination_zone destination_zone_id, const std::shared_ptr<transport>& transport_ptr)
    {
        RPC_ASSERT(destination_zone_id.get_val());
        RPC_ASSERT(transports_.find(destination_zone_id) == transports_.end());
        transports_[destination_zone_id] = transport_ptr;
        RPC_DEBUG("inner_add_transport service zone: {} destination_zone={} adjacent_zone={}",
            std::to_string(zone_id_),
            std::to_string(destination_zone_id),
            std::to_string(transport_ptr->get_adjacent_zone_id()));
    }

    void service::inner_remove_transport(destination_zone destination_zone_id)
    {
        RPC_ASSERT(destination_zone_id.get_val());
        auto it = transports_.find(destination_zone_id);
        if (it != transports_.end())
        {
            auto dest = it->second.lock();
            RPC_ASSERT(dest);
            transports_.erase(it);
            RPC_DEBUG("remove_transport service zone: {} destination_zone_id={}",
                std::to_string(zone_id_),
                std::to_string(destination_zone_id));
        }
    }
    std::shared_ptr<rpc::transport> service::inner_get_transport(destination_zone destination_zone_id) const
    {
        RPC_ASSERT(destination_zone_id.get_val());
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
            zone_id_.get_val(),
            destination_zone_id.get_val(),
            caller_zone_id.is_set() ? caller_zone_id.get_val() : 0,
            transports_.size());

        {
            auto item = service_proxies_.find(destination_zone_id);
            if (item != service_proxies_.end())
            {
                RPC_DEBUG("get_zone_proxy: Found existing proxy in service_proxies_");
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
                    auto proxy = service_proxy::create(fmt::format("SP#{}", destination_zone_id.get_val()),
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
            auto item = transports_.find(caller_zone_id.as_destination());
            if (item != transports_.end())
            {
                auto transport = item->second.lock();
                if (transport)
                {
                    auto proxy = service_proxy::create(fmt::format("SP#{}", destination_zone_id.get_val()),
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
            zone_id_.get_val(),
            destination_zone_id.get_val(),
            caller_zone_id.is_set() ? caller_zone_id.get_val() : 0);

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
            RPC_ASSERT(false);
        }
        else
        {
            service_proxies_.erase(item);
        }
    }

    int service::create_interface_stub(rpc::interface_ordinal interface_id,
        std::function<interface_ordinal(uint8_t)> interface_getter,
        const std::shared_ptr<rpc::i_interface_stub>& original,
        std::shared_ptr<rpc::i_interface_stub>& new_stub)
    {
        // an identity check, send back the same pointer
        if (interface_getter(rpc::VERSION_2) == interface_id)
        {
            new_stub = std::static_pointer_cast<rpc::i_interface_stub>(original);
            return rpc::error::OK();
        }

        auto it = stub_factories_.find(interface_id);
        if (it == stub_factories_.end())
        {
            RPC_INFO("stub factory does not have a record of this interface this not an error in the rpc stack");
            return rpc::error::INVALID_CAST();
        }

        new_stub = (*it->second)(original);
        if (!new_stub)
        {
            RPC_INFO("Object does not support the interface this not an error in the rpc stack");
            return rpc::error::INVALID_CAST();
        }
        // note a nullptr return value is a valid value, it indicates that this object does not implement that interface
        return rpc::error::OK();
    }

    // note this function is not thread safe!  Use it before using the service class for normal operation
    void service::add_interface_stub_factory(std::function<interface_ordinal(uint8_t)> id_getter,
        std::shared_ptr<std::function<std::shared_ptr<rpc::i_interface_stub>(const std::shared_ptr<rpc::i_interface_stub>&)>> factory)
    {
        auto interface_id = id_getter(rpc::VERSION_2);
        auto it = stub_factories_.find({interface_id});
        if (it != stub_factories_.end())
        {
            RPC_ERROR("Invalid data - add_interface_stub_factory failed");
            rpc::error::INVALID_DATA();
        }
        stub_factories_[{interface_id}] = factory;
    }

    rpc::shared_ptr<casting_interface> service::get_castable_interface(object object_id, interface_ordinal interface_id)
    {
        auto ob = get_object(object_id).lock();
        if (!ob)
            return nullptr;
        auto interface_stub = ob->get_interface(interface_id);
        if (!interface_stub)
            return nullptr;
        return interface_stub->get_castable_interface();
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
        if (!service_events_.empty())
        {
            auto service_events_copy = service_events_;
            for (auto se : service_events_copy)
            {
                auto se_handler = se.lock();
                if (se_handler)
                    CO_AWAIT se_handler->on_object_released(object_id, destination);
            }
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

    ///////////////////////////////////////////////////////////////////////////////
    // Default implementations of outbound functions
    ///////////////////////////////////////////////////////////////////////////////

    CORO_TASK(int)
    service::outbound_send(uint64_t protocol_version,
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
        const std::shared_ptr<transport>& transport)
    {
        // Default implementation - directly call the transport
        CO_RETURN CO_AWAIT transport->send(protocol_version,
            encoding,
            tag,
            caller_zone_id,
            destination_zone_id,
            object_id,
            interface_id,
            method_id,
            in_data,
            out_buf_,
            in_back_channel,
            out_back_channel);
    }

    CORO_TASK(void)
    service::outbound_post(uint64_t protocol_version,
        encoding encoding,
        uint64_t tag,
        caller_zone caller_zone_id,
        destination_zone destination_zone_id,
        object object_id,
        interface_ordinal interface_id,
        method method_id,
        const rpc::span& in_data,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        const std::shared_ptr<transport>& transport)
    {
        // Default implementation - directly call the transport
        CO_AWAIT transport->post(protocol_version,
            encoding,
            tag,
            caller_zone_id,
            destination_zone_id,
            object_id,
            interface_id,
            method_id,
            in_data,
            in_back_channel);
    }

    CORO_TASK(int)
    service::outbound_try_cast(uint64_t protocol_version,
        caller_zone caller_zone_id,
        destination_zone destination_zone_id,
        object object_id,
        interface_ordinal interface_id,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel,
        const std::shared_ptr<transport>& transport)
    {
        // Default implementation - directly call the transport
        CO_RETURN CO_AWAIT transport->try_cast(
            protocol_version, caller_zone_id, destination_zone_id, object_id, interface_id, in_back_channel, out_back_channel);
    }

    CORO_TASK(int)
    service::outbound_add_ref(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        known_direction_zone known_direction_zone_id,
        add_ref_options build_out_param_channel,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel,
        const std::shared_ptr<transport>& transport)
    {
        // Default implementation - directly call the transport
        CO_RETURN CO_AWAIT transport->add_ref(protocol_version,
            destination_zone_id,
            object_id,
            caller_zone_id,
            known_direction_zone_id,
            build_out_param_channel,
            in_back_channel,
            out_back_channel);
    }

    CORO_TASK(int)
    service::outbound_release(uint64_t protocol_version,
        destination_zone destination_zone_id,
        object object_id,
        caller_zone caller_zone_id,
        release_options options,
        const std::vector<rpc::back_channel_entry>& in_back_channel,
        std::vector<rpc::back_channel_entry>& out_back_channel,
        const std::shared_ptr<transport>& transport)
    {
        // Default implementation - directly call the transport
        CO_RETURN CO_AWAIT transport->release(
            protocol_version, destination_zone_id, object_id, caller_zone_id, options, in_back_channel, out_back_channel);
    }

    // Efficient targeted cleanup for a specific remote zone
    // This version is called when the transport knows exactly which zone connection failed
    CORO_TASK(void)
    service::notify_transport_down([[maybe_unused]] const std::shared_ptr<transport>& transport, destination_zone remote_zone)
    {
        std::lock_guard l(stub_control_);

        std::vector<interface_descriptor> objects_to_notify;

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

            bool should_delete = stub->release_all_from_zone(remote_zone.as_caller());

            if (should_delete)
            {
                // Shared count reached zero - stub should be deleted
                RPC_INFO("transport_down: Object {} ref count from zone {} dropped to zero, cleaning up",
                    obj_id.get_val(),
                    remote_zone.get_val());

                // Remove from maps
                stubs_.erase(obj_id);
                auto* pointer = stub->get_castable_interface()->get_address();
                wrapped_object_to_stub_.erase(pointer);

                // Track for notification
                objects_to_notify.push_back({obj_id, remote_zone});

                stub->reset();
            }
        }

        // Notify service events about deleted objects (outside the lock)
        for (const auto& obj : objects_to_notify)
        {
            CO_AWAIT notify_object_gone_event(obj.object_id, obj.destination_zone_id);
        }
    }

    template<class T>
    std::function<std::shared_ptr<rpc::i_interface_stub>(const std::shared_ptr<object_stub>&)>
    service::get_interface_stub_factory(const shared_ptr<T>&)
    {
        auto interface_id = T::get_id(rpc::VERSION_2);
        return [interface_id](const std::shared_ptr<object_stub>& stub) -> std::shared_ptr<rpc::i_interface_stub>
        {
            if (!stub)
                return nullptr;
            return stub->get_interface(interface_id);
        };
    }

    template<class T>
    std::function<std::shared_ptr<rpc::i_interface_stub>(const std::shared_ptr<object_stub>&)>
    service::get_interface_stub_factory(const optimistic_ptr<T>&)
    {
        auto interface_id = T::get_id(rpc::VERSION_2);
        return [interface_id](const std::shared_ptr<object_stub>& stub) -> std::shared_ptr<rpc::i_interface_stub>
        {
            if (!stub)
                return nullptr;
            return stub->get_interface(interface_id);
        };
    }

}
