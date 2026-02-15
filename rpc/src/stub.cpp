/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#include <rpc/rpc.h>

namespace rpc
{
    object_stub::object_stub(object id, const std::shared_ptr<service>& zone, [[maybe_unused]] void* target)
        : id_(id)
        , zone_(zone)
    {
#ifdef CANOPY_USE_TELEMETRY
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_stub_creation(zone_->get_zone_id(), id_, (uint64_t)target);
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

    rpc::shared_ptr<rpc::casting_interface> object_stub::get_castable_interface() const
    {
        std::lock_guard g(map_control_);
        RPC_ASSERT(!stub_map_.empty());
        auto& iface = stub_map_.begin()->second;
        return iface->get_castable_interface();
    }

    // this method is not thread safe as it is only used when the object is constructed by service
    // or by an internal call by this class
    void object_stub::add_interface(const std::shared_ptr<rpc::i_interface_stub>& iface)
    {
        stub_map_[iface->get_interface_id(rpc::VERSION_2)] = iface;
    }

    std::shared_ptr<rpc::i_interface_stub> object_stub::get_interface(interface_ordinal interface_id)
    {
        std::lock_guard g(map_control_);
        auto res = stub_map_.find(interface_id);
        if (res == stub_map_.end())
            return nullptr;
        return res->second;
    }

    CORO_TASK(int)
    object_stub::call(uint64_t protocol_version,
        rpc::encoding enc,
        caller_zone caller_zone_id,
        interface_ordinal interface_id,
        method method_id,
        const rpc::span& in_data,
        std::vector<char>& out_buf_)
    {
        std::shared_ptr<rpc::i_interface_stub> stub;
        {
            std::lock_guard g(map_control_);
            auto item = stub_map_.find(interface_id);
            if (item != stub_map_.end())
            {
                stub = item->second;
            }
        }
        if (stub)
        {
            CO_RETURN CO_AWAIT stub->call(protocol_version, enc, caller_zone_id, method_id, in_data, out_buf_);
        }
        RPC_ERROR("Invalid interface ID in stub call");
        CO_RETURN rpc::error::INVALID_INTERFACE_ID();
    }

    int object_stub::try_cast(interface_ordinal interface_id)
    {
        std::lock_guard g(map_control_);
        int ret = rpc::error::OK();
        auto item = stub_map_.find(interface_id);
        if (item == stub_map_.end())
        {
            std::shared_ptr<rpc::i_interface_stub> new_stub;
            std::shared_ptr<rpc::i_interface_stub> stub = stub_map_.begin()->second;
            ret = stub->cast(interface_id, new_stub);
            if (ret == rpc::error::OK() && new_stub)
            {
                add_interface(new_stub);
            }
        }
        return ret;
    }

    CORO_TASK(int) object_stub::add_ref(bool is_optimistic, bool outcall, caller_zone caller_zone_id)
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

        uint64_t ret = 0;
        auto transport = zone_->get_transport(caller_zone_id.as_destination());
        if (transport)
        {
            transport->increment_inbound_stub_count(caller_zone_id);

            if (outcall)
            {
                std::vector<rpc::back_channel_entry> out_back_channel;
                ret = CO_AWAIT transport->add_ref(rpc::get_version(),
                    get_zone()->get_zone_id().as_destination(),
                    id_,
                    caller_zone_id,
                    get_zone()->get_zone_id().as_known_direction_zone(),
                    rpc::add_ref_options::build_caller_route,
                    {},
                    out_back_channel);
            }
        }
        else
        {
            ret = error::TRANSPORT_ERROR();
            RPC_ASSERT(false);
            RPC_ERROR("Failed to find transport to increment inbound stub count");
        }

        CO_RETURN ret;
    }

    uint64_t object_stub::release(bool is_optimistic, caller_zone caller_zone_id)
    {
        uint64_t count = 0;
        if (is_optimistic)
        {
            // Update optimistic reference count for this caller zone
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
                    }
                }
                else
                {
                    RPC_ERROR("object stub does not know about this optimistic connection");
                }
            }
            count = --optimistic_count_;
        }
        else
        {
            // Update shared reference count for this caller zone
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
                    }
                }
                else
                {
                    RPC_ERROR("object stub does not know about this shared connection");
                }
            }
            count = --shared_count_;
        }
#if defined(CANOPY_USE_TELEMETRY) && defined(CANOPY_USE_TELEMETRY_RAII_LOGGING)
        if (auto telemetry_service = rpc::get_telemetry_service(); telemetry_service)
            telemetry_service->on_stub_release(zone_->get_zone_id(), id_, {}, count, {});
#endif
        RPC_ASSERT(count != std::numeric_limits<uint64_t>::max());
        auto transport = zone_->get_transport(caller_zone_id.as_destination());
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

    void object_stub::release_from_service(caller_zone caller_zone_id)
    {
        zone_->release_local_stub(shared_from_this(), false, caller_zone_id);
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
            RPC_DEBUG("release_all_from_zone: Released {} shared refs from zone {} for object {}",
                shared_refs_to_release,
                caller_zone_id.get_val(),
                id_.get_val());
        }

        if (optimistic_refs_to_release > 0)
        {
            optimistic_count_.fetch_sub(optimistic_refs_to_release, std::memory_order_acq_rel);
            RPC_DEBUG("release_all_from_zone: Released {} optimistic refs from zone {} for object {}",
                optimistic_refs_to_release,
                caller_zone_id.get_val(),
                id_.get_val());
        }

        // // Decrement transport counts
        // if (shared_refs_to_release + optimistic_refs_to_release > 0)
        // {
        //     auto transport = zone_->get_transport(caller_zone_id.as_destination());
        //     if (transport)
        //     {
        //         // Decrement once for each reference released
        //         for (uint64_t i = 0; i < shared_refs_to_release + optimistic_refs_to_release; ++i)
        //         {
        //             transport->decrement_inbound_stub_count(caller_zone_id);
        //         }
        //     }
        // }

        // Return true if shared count is now zero (stub should be deleted)
        return shared_count_.load(std::memory_order_acquire) == 0;
    }

}
