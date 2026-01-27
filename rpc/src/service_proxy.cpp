/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

// This file has been refactored to route communication through the service class.
// The service_proxy class now calls virtual 'outbound_' prefixed functions on the
// rpc::service class instead of directly calling the transport. This allows derived
// versions of the service class to add extra functionality to the overridden version,
// such as adding or processing back_channel data.
#include <rpc/rpc.h>
#include <cstdio>
#include <limits>
#include <algorithm>

namespace rpc
{
    service_proxy::service_proxy(const std::string& name,
        const zone zone_id,
        destination_zone destination_zone_id,
        std::shared_ptr<service> service,
        const std::shared_ptr<transport>& transport,
        uint64_t version,
        encoding enc)
        : name_(name)
        , zone_id_(zone_id)
        , destination_zone_id_(destination_zone_id)
        , service_(service)
        , transport_(transport)
        , version_(version)
        , enc_(enc)
    {
    }

    std::shared_ptr<service_proxy> service_proxy::create(const std::string& name,
        std::shared_ptr<service> service,
        const std::shared_ptr<transport>& transport,
        destination_zone destination_zone_id)
    {
        auto ret = std::shared_ptr<service_proxy>(new service_proxy(
            name, service->get_zone_id(), destination_zone_id, service, transport, rpc::get_version(), RPC_DEFAULT_ENCODING));
        transport->increment_outbound_proxy_count(destination_zone_id);
        return ret;
    }

    service_proxy::~service_proxy()
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_proxy_deletion(get_zone_id(), destination_zone_id_, zone_id_.as_caller());
        }
#endif

        // keep service alive while service proxy cleans things up
        auto service = service_;

        auto transport = transport_.get_nullable();
        if (transport)
            transport->decrement_outbound_proxy_count(destination_zone_id_);

        RPC_ASSERT(proxies_.empty());
        service->remove_zone_proxy(destination_zone_id_);

        if (!proxies_.empty())
        {
#ifdef USE_CANOPY_LOGGING
            RPC_WARNING("service_proxy destructor: {} proxies still in map for destination_zone={}",
                proxies_.size(),
                destination_zone_id_.get_val());

            // Log details of remaining proxies
            for (const auto& proxy_entry : proxies_)
            {
                auto proxy = proxy_entry.second.lock();
                RPC_WARNING(
                    "  Remaining proxy: object_id={}, valid={}", proxy_entry.first.get_val(), (proxy ? "true" : "false"));
            }
#endif
        }

        RPC_ASSERT(proxies_.empty());
    }

    void service_proxy::update_remote_rpc_version(uint64_t version)
    {
        const auto min_version = std::max<std::uint64_t>(rpc::LOWEST_SUPPORTED_VERSION, 1);
        const auto max_version = rpc::HIGHEST_SUPPORTED_VERSION;
        version_.store(std::clamp(version, min_version, max_version));
    }

    [[nodiscard]] CORO_TASK(int) service_proxy::send_from_this_zone(uint64_t protocol_version,
        rpc::encoding encoding,
        uint64_t tag,
        rpc::object object_id,
        rpc::interface_ordinal interface_id,
        rpc::method method_id,
        const rpc::span& in_data,
        std::vector<char>& out_buf_)
    {
        const auto min_version = std::max<std::uint64_t>(rpc::LOWEST_SUPPORTED_VERSION, 1);
        const auto max_version = rpc::HIGHEST_SUPPORTED_VERSION;
        if (protocol_version < min_version || protocol_version > max_version)
        {
            CO_RETURN rpc::error::INVALID_VERSION();
        }

        auto current_version = version_.load();
        if (protocol_version > current_version)
        {
            CO_RETURN rpc::error::INVALID_VERSION();
        }
        if (protocol_version < current_version)
        {
            version_.store(protocol_version);
        }

        auto transport = transport_.get_nullable();
        if (!transport)
        {
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        std::vector<rpc::back_channel_entry> empty_in;
        std::vector<rpc::back_channel_entry> empty_out;
        // Call the outbound function on the service to allow derived classes to add extra functionality
        // such as processing back_channel data
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_proxy_send(
                get_zone_id(), destination_zone_id_, get_zone_id().as_caller(), object_id, interface_id, method_id);
        }
#endif
        CO_RETURN CO_AWAIT service_->outbound_send(protocol_version,
            encoding,
            tag,
            get_zone_id().as_caller(),
            destination_zone_id_,
            object_id,
            interface_id,
            method_id,
            in_data,
            out_buf_,
            empty_in,
            empty_out,
            transport);
    }

    [[nodiscard]] CORO_TASK(int) service_proxy::sp_try_cast(
        destination_zone destination_zone_id, object object_id, std::function<interface_ordinal(uint64_t)> id_getter)
    {
        auto original_version = version_.load();
        auto version = original_version;
        const auto min_version = rpc::LOWEST_SUPPORTED_VERSION ? rpc::LOWEST_SUPPORTED_VERSION : 1;
        int last_error = rpc::error::INVALID_VERSION();

        auto transport = transport_.get_nullable();
        if (!transport)
        {
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        while (version >= min_version)
        {
            auto if_id = id_getter(version);

#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                telemetry_service->on_service_proxy_try_cast(
                    get_zone_id(), destination_zone_id, get_zone_id().as_caller(), object_id, if_id);
            }
#endif

            std::vector<rpc::back_channel_entry> empty_in;
            std::vector<rpc::back_channel_entry> empty_out;
            // Call the outbound function on the service to allow derived classes to add extra functionality
            // such as processing back_channel data
            auto ret = CO_AWAIT service_->outbound_try_cast(
                version, get_zone_id().as_caller(), destination_zone_id, object_id, if_id, empty_in, empty_out, transport);
            if (ret != rpc::error::INVALID_VERSION() && ret != rpc::error::INCOMPATIBLE_SERVICE())
            {
                if (original_version != version)
                {
                    version_.compare_exchange_strong(original_version, version);
                }
                CO_RETURN ret;
            }
            last_error = ret;
            if (version == min_version)
            {
                break;
            }
            version--;
        }
        RPC_ERROR("Incompatible service version in try_cast");
        CO_RETURN last_error;
    }

    [[nodiscard]] CORO_TASK(int) service_proxy::sp_add_ref(
        object object_id, add_ref_options build_out_param_channel, known_direction_zone known_direction_zone_id)
    {
        auto transport = transport_.get_nullable();
        if (!transport)
        {
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        auto original_version = version_.load();
        auto version = original_version;
        const auto min_version = rpc::LOWEST_SUPPORTED_VERSION ? rpc::LOWEST_SUPPORTED_VERSION : 1;
        int last_error = rpc::error::INVALID_VERSION();
        while (version >= min_version)
        {
            std::vector<rpc::back_channel_entry> empty_in;
            std::vector<rpc::back_channel_entry> empty_out;
            // Call the outbound function on the service to allow derived classes to add extra functionality
            // such as processing back_channel data

            auto attempt = CO_AWAIT service_->outbound_add_ref(version,
                destination_zone_id_,
                object_id,
                zone_id_.as_caller(),
                known_direction_zone_id,
                build_out_param_channel,
                empty_in,
                empty_out,
                transport);
            if (attempt != rpc::error::INVALID_VERSION() && attempt != rpc::error::INCOMPATIBLE_SERVICE())
            {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
                if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                {
                    telemetry_service->on_service_proxy_add_ref(get_zone_id(),
                        destination_zone_id_,
                        zone_id_.as_caller(),
                        object_id,
                        known_direction_zone_id,
                        build_out_param_channel);
                }
#endif
                if (original_version != version)
                {
                    version_.compare_exchange_strong(original_version, version);
                }
                CO_RETURN attempt;
            }
            last_error = attempt;
            if (version == min_version)
            {
                break;
            }
            version--;
        }
        RPC_ERROR("Incompatible service version in sp_add_ref");
        CO_RETURN last_error;
    }

    CORO_TASK(int) service_proxy::sp_release(object object_id, release_options options)
    {
        auto transport = transport_.get_nullable();
        if (!transport)
        {
            CO_RETURN rpc::error::TRANSPORT_ERROR();
        }

        auto original_version = version_.load();
        auto version = original_version;
        const auto min_version = rpc::LOWEST_SUPPORTED_VERSION ? rpc::LOWEST_SUPPORTED_VERSION : 1;
        int last_error = rpc::error::INVALID_VERSION();

        while (version >= min_version)
        {
            std::vector<rpc::back_channel_entry> empty_in;
            std::vector<rpc::back_channel_entry> empty_out;
            // Call the outbound function on the service to allow derived classes to add extra functionality
            // such as processing back_channel data
            auto ret = CO_AWAIT service_->outbound_release(
                version, destination_zone_id_, object_id, zone_id_.as_caller(), options, empty_in, empty_out, transport);
            if (ret != rpc::error::INVALID_VERSION() && ret != rpc::error::INCOMPATIBLE_SERVICE())
            {
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
                if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                {
                    telemetry_service->on_service_proxy_release(
                        get_zone_id(), destination_zone_id_, zone_id_.as_caller(), object_id, options);
                }
#endif
                if (original_version != version)
                {
                    version_.compare_exchange_strong(original_version, version);
                }
                CO_RETURN ret;
            }
            last_error = ret;
            if (version == min_version)
            {
                break;
            }
            version--;
        }
        RPC_ERROR("Incompatible service version in sp_release");
        CO_RETURN last_error;
    }

    CORO_TASK(void)
    send_object_release(std::shared_ptr<rpc::service> svc,
        rpc::object object_id,
        std::shared_ptr<rpc::transport> transport,
        uint64_t version,
        destination_zone destination_zone_id,
        bool is_optimistic)
    {
        RPC_DEBUG("send_object_release starting release for object {}", object_id.get_val());

        auto caller_id = svc->get_zone_id().as_caller();
        // Release our reference to the service, allowing it to be destroyed if no longer needed
        // if service is released then the last thing standing is the transport which may then tell its counterpart
        // to begin cleanup too

        std::vector<rpc::back_channel_entry> empty_in;
        std::vector<rpc::back_channel_entry> empty_out;

        // Call the outbound function on the service to allow derived classes to add extra functionality
        // such as processing back_channel data
        auto ret = CO_AWAIT svc->outbound_release(version,
            destination_zone_id,
            object_id,
            caller_id,
            is_optimistic ? release_options::optimistic : release_options::normal,
            empty_in,
            empty_out,
            transport);
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            telemetry_service->on_service_proxy_release(svc->get_zone_id(),
                destination_zone_id,
                svc->get_zone_id().as_caller(),
                object_id,
                is_optimistic ? release_options::optimistic : release_options::normal);
        }
#endif

        // Notify that object is gone after all cleanup is complete
        CO_AWAIT svc->notify_object_gone_event(object_id, destination_zone_id);

        RPC_DEBUG(
            "send_object_release: release returned {} for object {}", rpc::error::to_string(ret), object_id.get_val());

        // error handling here as the cleanup needs to happen anyway
        if (ret == rpc::error::OK())
        {
            RPC_DEBUG("Remote {} for object {}", is_optimistic ? "optimistic" : "shared", object_id.get_val());
        }
        else if (is_optimistic && ret == rpc::error::OBJECT_NOT_FOUND())
        {
            RPC_DEBUG("Object {} not found - stub already deleted (normal for optimistic_ptr)", object_id.get_val());
        }
        else if (ret == rpc::error::ZONE_NOT_FOUND() || ret == rpc::error::TRANSPORT_ERROR())
        {
            RPC_DEBUG("Zone {} not reachable during cleanup ({}), intermediate zone may have been cleaned up "
                      "(normal during multi-level hierarchy cleanup)",
                destination_zone_id.get_val(),
                rpc::error::to_string(ret));
        }
        else
        {
            RPC_ERROR("cleanup_after_object release failed: {}", rpc::error::to_string(ret));
            RPC_ASSERT(false);
        }
        svc.reset();
        // the transport may now go out of scope and be destroyed here

        CO_RETURN;
    }

    void service_proxy::on_object_proxy_released(const std::shared_ptr<object_proxy>& op, bool is_optimistic)
    {
        auto object_id = op->get_object_id();

        RPC_DEBUG("on_object_proxy_released service zone: {} destination_zone={}, object_id = {} "
                  "decrement={})",
            get_zone_id().get_val(),
            destination_zone_id_.get_val(),
            object_id.get_val(),
            is_optimistic ? "optimistic" : "shared");

#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
        {
            auto transport = transport_.get_nullable();
            RPC_ASSERT(transport);
            telemetry_service->on_service_proxy_release(get_zone_id(),
                destination_zone_id_,
                zone_id_.as_caller(),
                object_id,
                is_optimistic ? release_options::optimistic : release_options::normal);
        }
#endif

        {
            // as there are no more refcounts on the object proxy remove it from the proxies_ map now
            // it is not possible to know if it is time to remove the proxies_ map deterministically once we
            std::lock_guard proxy_lock(insert_control_);
            if (op->get_shared_count() == 0 && op->get_optimistic_count() == 0)
            {
                auto item = proxies_.find(object_id);
                RPC_ASSERT(item != proxies_.end());
                if (item != proxies_.end())
                {
                    // Remove from map since object_proxy is being destroyed
                    RPC_DEBUG("Removing object_id={} from proxy map (object_proxy being destroyed)", object_id.get_val());
                    proxies_.erase(item);
                }
            }
        }

        RPC_DEBUG("cleanup_after_object service zone: {} destination_zone={}, object_id = {} "
                  "decrement={}",
            get_zone_id().get_val(),
            destination_zone_id_.get_val(),
            object_id.get_val(),
            is_optimistic ? "optimistic" : "shared");

        auto transport = transport_.get_nullable();
        if (!transport)
        {
            RPC_ERROR("transport_ is null unable to release");
            RPC_ASSERT(false);
            return;
        }

        auto version = version_.load();
        auto destination_zone_id = destination_zone_id_;
        auto svc = get_operating_zone_service();

#ifdef CANOPY_BUILD_COROUTINE
        // DO NOT pass service_proxy - it would create circular reference keeping services alive!
        if (!svc->spawn(send_object_release(svc, object_id, transport, version, destination_zone_id, is_optimistic)))
        {
            RPC_ERROR("Failed to spawn coroutine to send object release for object {}", object_id.get_val());
            RPC_ASSERT(false);
        }
#else
        send_object_release(svc, object_id, transport, version, destination_zone_id, is_optimistic);
#endif
    }

    CORO_TASK(int)
    service_proxy::get_or_create_object_proxy(object object_id,
        object_proxy_creation_rule rule,
        bool new_proxy_added,
        known_direction_zone known_direction_zone_id,
        bool is_optimistic,
        std::shared_ptr<rpc::object_proxy>& op)
    {
        RPC_DEBUG("get_or_create_object_proxy service zone: {} destination_zone={}, caller_zone={}, object_id = {}",
            zone_id_.get_val(),
            destination_zone_id_.get_val(),
            zone_id_.as_caller().get_val(),
            object_id.get_val());

        std::shared_ptr<rpc::object_proxy> tmp;
        bool is_new = false;
        std::shared_ptr<rpc::service_proxy> self_ref; // Hold reference to prevent destruction

        {
            std::lock_guard l(insert_control_);

            // Capture strong reference to self while in critical section

            auto item = proxies_.find(object_id);
            if (item != proxies_.end())
            {
                tmp = item->second.lock();
            }

            if (!tmp)
            {
                // Either no entry exists, or the weak_ptr is expired - create new object_proxy
                tmp = std::shared_ptr<rpc::object_proxy>(new object_proxy(object_id, shared_from_this()));
#ifdef CANOPY_USE_TELEMETRY
                if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
                {
                    telemetry_service->on_object_proxy_creation(get_zone_id(), get_destination_zone_id(), object_id, true);
                }
#endif
                proxies_[object_id] = tmp;
                is_new = true;
            }
        }

        // Perform remote operations OUTSIDE the mutex lock
        // self_ref keeps this service_proxy alive during these operations
        if (is_new && rule == object_proxy_creation_rule::ADD_REF_IF_NEW)
        {
#ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            {
                telemetry_service->message(rpc::i_telemetry_service::level_enum::info,
                    "get_or_create_object_proxy calling sp_add_ref with normal options for new object_proxy");
            }
#endif
            auto ret = CO_AWAIT sp_add_ref(object_id,
                is_optimistic ? rpc::add_ref_options::optimistic : rpc::add_ref_options::normal,
                known_direction_zone_id);
            if (ret != error::OK())
            {
                RPC_ERROR("sp_add_ref failed");
                std::lock_guard l(insert_control_);
                proxies_.erase(object_id);
                RPC_ASSERT(false);
                CO_RETURN ret;
            }
        }
        if (!is_new && rule == object_proxy_creation_rule::RELEASE_IF_NOT_NEW)
        {
            RPC_DEBUG(
                "get_or_create_object_proxy calling sp_release due to object_proxy_creation_rule::RELEASE_IF_NOT_NEW");

            // as this is an out parameter the callee will be doing an add ref if the object proxy is already
            // found we can do a release
            RPC_ASSERT(!new_proxy_added);
            auto ret = CO_AWAIT sp_release(
                object_id, is_optimistic ? rpc::release_options::optimistic : rpc::release_options::normal);
            if (ret != error::OK())
            {
                RPC_ERROR("sp_release failed");
                CO_RETURN ret;
            }
        }

        op = tmp;

        // self_ref goes out of scope here, allowing normal destruction if needed
        CO_RETURN error::OK();
    }
}
