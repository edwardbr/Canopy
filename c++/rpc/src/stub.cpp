/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#include <rpc/rpc.h>
#include <cstdint>

namespace rpc
{
    object_stub::object_stub(
        object id,
        const std::shared_ptr<service>& zone,
        const rpc::shared_ptr<rpc::casting_interface>& target)
        : id_(id)
        , target_(target)
        , zone_(zone)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_stub_creation(zone_->get_zone_id(), id_, reinterpret_cast<std::uintptr_t>(target.get()));
#endif
#ifdef CANOPY_USE_LOGGING
#  if CANOPY_LOGGING_LEVEL <= 1
        auto tmp = zone->get_zone_id().with_object(id);
        if (!tmp)
        {
            RPC_ERROR("{}", tmp.error());
        }
        else
        {
            RPC_DEBUG("New object_stub {}", rpc::to_string(tmp.value()));
        }
#  endif
#endif
    }
    object_stub::~object_stub()
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_stub_deletion(zone_->get_zone_id(), id_);
#endif
        RPC_ASSERT(shared_count_ == 0);
    }

    rpc::shared_ptr<rpc::casting_interface> object_stub::get_castable_interface(interface_ordinal interface_id) const
    {
        if (!target_)
            return nullptr;
        if (!interface_id.is_set())
            return target_;

        auto* iface = const_cast<rpc::casting_interface*>(target_->__rpc_query_interface(interface_id));
        if (!iface)
            return nullptr;
        return rpc::shared_ptr<rpc::casting_interface>(target_, iface);
    }

    CORO_TASK(send_result)
    object_stub::call(send_params params)
    {
        if (target_)
        {
            CO_RETURN CO_AWAIT target_->__rpc_call(std::move(params));
        }
        RPC_ERROR("Invalid interface ID in stub call");
        CO_RETURN send_result{rpc::error::INVALID_INTERFACE_ID(), {}, {}};
    }

    int object_stub::try_cast(interface_ordinal interface_id)
    {
        if (!target_)
            return rpc::error::OBJECT_NOT_FOUND();

        auto* iface = const_cast<rpc::casting_interface*>(target_->__rpc_query_interface(interface_id));
        if (!iface)
            return rpc::error::INVALID_CAST();

        return rpc::error::OK();
    }

    CORO_TASK(int)
    object_stub::add_ref(
        bool is_optimistic,
        bool outcall,
        caller_zone caller_zone_id,
        uint64_t request_id)
    {
        uint64_t count = 0;
        if (is_optimistic)
        {
            // Track optimistic reference for this caller zone
            {
                std::lock_guard<std::mutex> lock(references_mutex_);
                auto& ref_count = optimistic_references_[caller_zone_id];
                ref_count.fetch_add(1, std::memory_order_acq_rel);
            }
            count = ++optimistic_count_;
        }
        else
        {
            // Track shared reference for this caller zone
            {
                std::lock_guard<std::mutex> lock(references_mutex_);
                auto& ref_count = shared_references_[caller_zone_id];
                ref_count.fetch_add(1, std::memory_order_acq_rel);
            }
            count = ++shared_count_;
        }
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_stub_add_ref(zone_->get_zone_id(), id_, {}, count, {});
#endif
        RPC_ASSERT(count != std::numeric_limits<uint64_t>::max());
        RPC_ASSERT(count != 0);

        uint64_t ret = error::OK();
        auto transport = zone_->get_transport(caller_zone_id);
        if (transport)
        {
            transport->increment_inbound_stub_count(caller_zone_id);

            if (outcall)
            {
                add_ref_params ar_params;
                ar_params.protocol_version = rpc::get_version();
                {
                    auto r = get_zone()->get_zone_id().with_object(id_);
                    if (!r)
                        CO_RETURN rpc::error::INVALID_DATA();
                    ar_params.remote_object_id = std::move(*r);
                }
                ar_params.caller_zone_id = caller_zone_id;
                ar_params.requesting_zone_id = get_zone()->get_zone_id();
                ar_params.build_out_param_channel = rpc::add_ref_options::build_caller_route
                                                    | (is_optimistic ? rpc::add_ref_options::optimistic
                                                                     : rpc::add_ref_options::normal);
                ar_params.request_id = request_id;
                auto ar_result = CO_AWAIT transport->add_ref(std::move(ar_params));
                ret = ar_result.error_code;
            }
        }
        else
        {
            ret = error::TRANSPORT_ERROR();
            RPC_ERROR("Failed to find transport to increment inbound stub count");
            RPC_ASSERT(false);
        }

        CO_RETURN ret;
    }

    uint64_t object_stub::release(
        bool is_optimistic,
        caller_zone caller_zone_id)
    {
        uint64_t count = 0;
        if (is_optimistic)
        {
            // Update optimistic reference count for this caller zone
            bool did_release = false;
            {
                std::lock_guard<std::mutex> lock(references_mutex_);
                auto it = optimistic_references_.find(caller_zone_id);
                if (it != optimistic_references_.end())
                {
                    auto& ref_count = it->second;
                    uint64_t current = ref_count.load(std::memory_order_acquire);
                    if (current > 0)
                    {
                        uint64_t new_count = ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
                        if (new_count == 0)
                        {
                            optimistic_references_.erase(it);
                        }
                        did_release = true;
                    }
                    else
                    {
                        RPC_ERROR("negative optimistic reference count");
                    }
                }
                else
                {
                    // Already cleaned up by release_all_from_zone (concurrent transport teardown)
                    RPC_WARNING("release: optimistic zone entry already removed (concurrent teardown), skipping decrement");
                }
            }
            if (did_release)
                count = --optimistic_count_;
            else
                count = optimistic_count_.load(std::memory_order_acquire);
        }
        else
        {
            // Update shared reference count for this caller zone
            bool did_release = false;
            {
                std::lock_guard<std::mutex> lock(references_mutex_);
                auto it = shared_references_.find(caller_zone_id);
                if (it != shared_references_.end())
                {
                    auto& ref_count = it->second;
                    uint64_t current = ref_count.load(std::memory_order_acquire);
                    if (current > 0)
                    {
                        uint64_t new_count = ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
                        if (new_count == 0)
                        {
                            shared_references_.erase(it);
                        }
                        did_release = true;
                    }
                    else
                    {
                        RPC_ERROR("negative shared reference count");
                    }
                }
                else
                {
                    // Already cleaned up by release_all_from_zone (concurrent transport teardown)
                    RPC_WARNING("release: shared zone entry already removed (concurrent teardown), skipping decrement");
                }
            }
            // Only decrement the global count if we actually consumed a per-zone reference.
            // If release_all_from_zone already erased the entry and decremented the count,
            // doing so again would underflow shared_count_ to uint64_t::max.
            if (did_release)
                count = --shared_count_;
            else
                count = shared_count_.load(std::memory_order_acquire);
        }
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_stub_release(zone_->get_zone_id(), id_, {}, count, {});
#endif
        RPC_ASSERT(count != std::numeric_limits<uint64_t>::max());
        auto transport = zone_->get_transport(caller_zone_id);
        if (transport)
        {
            transport->decrement_inbound_stub_count(caller_zone_id);
        }
        else
        {
            RPC_ERROR("Failed to find transport to decrement inbound stub count");
        }
        return count;
    }

    CORO_TASK(void) object_stub::release_from_service(caller_zone caller_zone_id)
    {
        CO_AWAIT zone_->release_local_stub(shared_from_this(), false, caller_zone_id);
        CO_RETURN;
    }

    bool object_stub::has_references_from_zone(caller_zone caller_zone_id) const
    {
        std::lock_guard<std::mutex> lock(references_mutex_);

        // Check shared references
        auto shared_it = shared_references_.find(caller_zone_id);
        if (shared_it != shared_references_.end())
        {
            if (shared_it->second.load(std::memory_order_acquire) > 0)
                return true;
        }

        // Check optimistic references
        auto opt_it = optimistic_references_.find(caller_zone_id);
        if (opt_it != optimistic_references_.end())
        {
            if (opt_it->second.load(std::memory_order_acquire) > 0)
                return true;
        }

        return false;
    }

    std::vector<caller_zone> object_stub::get_zones_with_optimistic_refs() const
    {
        std::lock_guard<std::mutex> lock(references_mutex_);
        std::vector<caller_zone> result;
        result.reserve(optimistic_references_.size());
        for (const auto& [zone, count_atomic] : optimistic_references_)
        {
            if (count_atomic.load(std::memory_order_acquire) > 0)
            {
                result.push_back(zone);
            }
        }
        return result;
    }

    bool object_stub::release_all_from_zone(caller_zone caller_zone_id)
    {
        uint64_t shared_refs_to_release = 0;
        uint64_t optimistic_refs_to_release = 0;

        // Get counts and remove from maps
        {
            std::lock_guard<std::mutex> lock(references_mutex_);

            // Get shared reference count for this zone
            auto shared_it = shared_references_.find(caller_zone_id);
            if (shared_it != shared_references_.end())
            {
                shared_refs_to_release = shared_it->second.load(std::memory_order_acquire);
                shared_references_.erase(shared_it);
            }

            // Get optimistic reference count for this zone
            auto opt_it = optimistic_references_.find(caller_zone_id);
            if (opt_it != optimistic_references_.end())
            {
                optimistic_refs_to_release = opt_it->second.load(std::memory_order_acquire);
                optimistic_references_.erase(opt_it);
            }
        }

        // Release the references (outside the lock to avoid issues)
        if (shared_refs_to_release > 0)
        {
            shared_count_.fetch_sub(shared_refs_to_release, std::memory_order_acq_rel);
            RPC_DEBUG(
                "release_all_from_zone: Released {} shared refs from zone {} for object {}",
                shared_refs_to_release,
                caller_zone_id.get_subnet(),
                id_.get_val());
        }

        if (optimistic_refs_to_release > 0)
        {
            optimistic_count_.fetch_sub(optimistic_refs_to_release, std::memory_order_acq_rel);
            RPC_DEBUG(
                "release_all_from_zone: Released {} optimistic refs from zone {} for object {}",
                optimistic_refs_to_release,
                caller_zone_id.get_subnet(),
                id_.get_val());
        }

        // Decrement transport counts in a single lock acquisition
        const uint64_t total_refs = shared_refs_to_release + optimistic_refs_to_release;
        if (total_refs > 0)
        {
            auto transport = zone_->get_transport(caller_zone_id);
            if (transport)
            {
                transport->decrement_inbound_stub_count_by(caller_zone_id, total_refs);
            }
        }

        // Return true if shared count is now zero (stub should be deleted)
        return shared_count_.load(std::memory_order_acquire) == 0;
    }

}
