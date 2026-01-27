/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
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
#include <rpc/telemetry/i_telemetry_service.h>
#endif
#include <rpc/internal/service.h>

namespace rpc
{

    class i_interface_stub;
    class object_stub;
    class service;
    class service_proxy;
    class casting_interface;

    class object_stub
    {
        object id_ = {0};
        // stubs have stong pointers
        mutable std::mutex map_control_;
        std::unordered_map<interface_ordinal, std::shared_ptr<rpc::i_interface_stub>> stub_map_;
        std::shared_ptr<object_stub> p_this_;
        std::atomic<uint64_t> shared_count_ = 0;
        std::atomic<uint64_t> optimistic_count_ = 0;
        // Track optimistic and shared references per zone for transport_down cleanup
        std::unordered_map<caller_zone, std::atomic<uint64_t>> optimistic_references_;
        std::unordered_map<caller_zone, std::atomic<uint64_t>> shared_references_;
        mutable std::mutex references_mutex_; // Protects both optimistic and shared reference maps
        std::shared_ptr<service> zone_;       // Strong reference to keep service alive while stub exists

        void add_interface(const std::shared_ptr<rpc::i_interface_stub>& iface);
        friend service; // so that it can call add_interface

    public:
        object_stub(object id, const std::shared_ptr<service>& zone, void* target);
        ~object_stub();
        object get_id() const { return id_; }
        rpc::shared_ptr<rpc::casting_interface> get_castable_interface() const;
        void reset() { p_this_.reset(); }

        // this is called once the lifetime management needs to be activated
        void on_added_to_zone(std::shared_ptr<object_stub> stub) { p_this_ = stub; }

        std::shared_ptr<service> get_zone() const { return zone_; }

        CORO_TASK(int)
        call(uint64_t protocol_version,
            rpc::encoding enc,
            caller_zone caller_zone_id,
            interface_ordinal interface_id,
            method method_id,
            const rpc::span& in_data,
            std::vector<char>& out_buf_);
        int try_cast(interface_ordinal interface_id);

        std::shared_ptr<rpc::i_interface_stub> get_interface(interface_ordinal interface_id);

        uint64_t add_ref(bool is_optimistic, bool outcall, caller_zone caller_zone_id);
        uint64_t release(bool is_optimistic, caller_zone caller_zone_id);
        void release_from_service(caller_zone caller_zone_id);

        // Returns true if this stub has any references (shared or optimistic) from the given zone
        bool has_references_from_zone(caller_zone caller_zone_id) const;

        // Release all references (both shared and optimistic) from a specific zone
        // Returns true if the stub should be deleted (shared count reached zero)
        bool release_all_from_zone(caller_zone caller_zone_id);
    };

    class i_interface_stub
    {
    public:
        virtual ~i_interface_stub() = default;
        virtual interface_ordinal get_interface_id(uint64_t rpc_version) const = 0;
        virtual CORO_TASK(int) call(uint64_t protocol_version,
            rpc::encoding enc,
            caller_zone caller_zone_id,
            method method_id,
            const rpc::span& in_data,
            std::vector<char>& out_buf_)
            = 0;
        virtual int cast(interface_ordinal interface_id, std::shared_ptr<rpc::i_interface_stub>& new_stub) = 0;
        virtual std::weak_ptr<rpc::object_stub> get_object_stub() const = 0;
        virtual void* get_pointer() const = 0;
        virtual rpc::shared_ptr<rpc::casting_interface> get_castable_interface() const = 0;
    };
}
