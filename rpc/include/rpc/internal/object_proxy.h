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
#include <functional>
#include <vector>
#include <type_traits>

#include <rpc/internal/types.h>
#include <rpc/internal/coroutine_support.h>
#include <rpc/internal/serialiser.h>
#include <rpc/internal/version.h>
#include <rpc/internal/remote_pointer.h>

// Forward declarations
namespace rpc
{
    template<class Ptr> struct query_interface_result
    {
        int error_code;
        Ptr iface;
    };

    class service;
    class service_proxy;
    class casting_interface;
    template<typename T> class weak_ptr;

    class object_proxy : public std::enable_shared_from_this<rpc::object_proxy>
    {
        object object_id_;
        stdex::member_ptr<service_proxy> service_proxy_;
        std::unordered_map<interface_ordinal, rpc::weak_ptr<casting_interface>> proxy_map;
        std::mutex insert_control_;
        // Track shared references from control block transitions (public for telemetry)
        std::atomic<int> shared_count_{0};
        // Track optimistic references from control block transitions (public for telemetry)
        std::atomic<int> optimistic_count_{0};

        object_proxy(object object_id, std::shared_ptr<rpc::service_proxy> service_proxy);

        // note the interface pointer may change if there is already an interface inserted successfully
        void register_interface(interface_ordinal interface_id, rpc::weak_ptr<casting_interface>& value);

        CORO_TASK(int) try_cast(std::function<interface_ordinal(uint64_t)> id_getter);

        friend service_proxy;

    public:
        // Track shared references from control block transitions (public for telemetry)
        int get_shared_count() const { return shared_count_.load(std::memory_order_acquire); }
        // Track optimistic references from control block transitions (public for telemetry)
        int get_optimistic_count() const { return optimistic_count_.load(std::memory_order_acquire); }

        ~object_proxy();

        // Reference counting methods for control block integration
        // add_ref is async because on 0→1 transitions it must call service_proxy->sp_add_ref()
        // to establish remote reference immediately and sequentially
        CORO_TASK(int) add_ref(add_ref_options options);
        // release is synchronous - just decrements local counters, cleanup happens in destructor
        void release(bool is_optimistic);

        // Called when this object_proxy inherits a shared reference from a race condition during the destruction of a
        void add_ref_shared() { shared_count_.fetch_add(1, std::memory_order_relaxed); }

        // Called when this object_proxy inherits an optimistic reference from a race condition during the destruction of a proxy
        void add_ref_optimistic() { optimistic_count_.fetch_add(1, std::memory_order_relaxed); }

        std::shared_ptr<rpc::service_proxy> get_service_proxy() const { return service_proxy_.get_nullable(); }
        object get_object_id() const { return {object_id_}; }
        destination_zone get_destination_zone_id() const;

        [[nodiscard]] CORO_TASK(send_result) send(uint64_t protocol_version,
            rpc::encoding encoding,
            uint64_t tag,
            rpc::interface_ordinal interface_id,
            rpc::method method_id,
            rpc::byte_span in_data);

        CORO_TASK(int)
        post(uint64_t protocol_version,
            rpc::encoding encoding,
            uint64_t tag,
            rpc::interface_ordinal interface_id,
            rpc::method method_id,
            rpc::byte_span in_data);

        size_t get_proxy_count()
        {
            std::lock_guard guard(insert_control_);
            return proxy_map.size();
        }

        template<class T> void create_interface_proxy(rpc::shared_ptr<T>& inface);

        template<class T, template<class> class PtrType = rpc::shared_ptr, bool default_do_remote_check = true>
        CORO_TASK(query_interface_result<PtrType<T>>)
        query_interface(bool do_remote_check = default_do_remote_check)
        {
            static_assert(__rpc_pointer_traits::is_supported_v<PtrType<T>>,
                "query_interface only supports rpc::shared_ptr and rpc::optimistic_ptr");
            query_interface_result<PtrType<T>> result{rpc::error::OK(), {}};

            const auto interface_id = T::get_id(rpc::get_version());

            { // scope for the lock
                if (interface_id == 0)
                {
                    CO_RETURN result;
                }

                std::unique_lock<std::mutex> guard(insert_control_);
                {
                    auto item = proxy_map.find(interface_id);
                    if (item != proxy_map.end())
                    {
                        auto proxy = rpc::reinterpret_pointer_cast<T>(item->second.lock());
                        if (!proxy)
                        {
                            // weak pointer needs refreshing
                            create_interface_proxy<T>(proxy);
                            item->second = rpc::reinterpret_pointer_cast<casting_interface>(proxy);
                        }

                        if constexpr (__rpc_pointer_traits::is_optimistic_v<PtrType<T>>)
                        {
                            guard.unlock();
                            // False positive: the lock is explicitly released before suspension.
                            // NOLINTNEXTLINE(cppcoreguidelines-no-suspend-with-lock)
                            std::tie(result.error_code, result.iface) = CO_AWAIT rpc::make_optimistic(proxy);
                            CO_RETURN result;
                        }
                        else
                        {
                            result.iface = proxy;
                            CO_RETURN result;
                        }
                    }
                }
                if (!do_remote_check)
                {
                    rpc::shared_ptr<T> tmp;
                    create_interface_proxy<T>(tmp);
                    proxy_map[interface_id] = rpc::reinterpret_pointer_cast<casting_interface>(tmp);

                    if constexpr (__rpc_pointer_traits::is_optimistic_v<PtrType<T>>)
                    {
                        guard.unlock();
                        // False positive: the lock is explicitly released before suspension.
                        // NOLINTNEXTLINE(cppcoreguidelines-no-suspend-with-lock)
                        std::tie(result.error_code, result.iface) = CO_AWAIT rpc::make_optimistic(tmp);
                        CO_RETURN result;
                    }
                    else
                    {
                        result.iface = tmp;
                        CO_RETURN result;
                    }
                }
            }

            // release the lock and then check for casting
            if (do_remote_check)
            {
                // see if object_id can implement interface
                int ret = CO_AWAIT try_cast(T::get_id);
                if (ret != rpc::error::OK())
                {
                    result.error_code = ret;
                    CO_RETURN result;
                }
            }
            { // another scope for the lock
                std::unique_lock<std::mutex> guard(insert_control_);

                // check again...
                {
                    auto item = proxy_map.find(interface_id);
                    if (item != proxy_map.end())
                    {
                        auto proxy = rpc::reinterpret_pointer_cast<T>(item->second.lock());
                        if (!proxy)
                        {
                            // weak pointer needs refreshing
                            create_interface_proxy<T>(proxy);
                            item->second = rpc::reinterpret_pointer_cast<casting_interface>(proxy);
                        }

                        if constexpr (__rpc_pointer_traits::is_optimistic_v<PtrType<T>>)
                        {
                            guard.unlock();
                            // False positive: the lock is explicitly released before suspension.
                            // NOLINTNEXTLINE(cppcoreguidelines-no-suspend-with-lock)
                            std::tie(result.error_code, result.iface) = CO_AWAIT rpc::make_optimistic(proxy);
                            CO_RETURN result;
                        }
                        else
                        {
                            result.iface = proxy;
                            CO_RETURN result;
                        }
                    }
                }
                rpc::shared_ptr<T> tmp;
                create_interface_proxy<T>(tmp);
                proxy_map[interface_id] = rpc::reinterpret_pointer_cast<casting_interface>(tmp);

                if constexpr (__rpc_pointer_traits::is_optimistic_v<PtrType<T>>)
                {
                    guard.unlock();
                    // False positive: the lock is explicitly released before suspension.
                    // NOLINTNEXTLINE(cppcoreguidelines-no-suspend-with-lock)
                    std::tie(result.error_code, result.iface) = CO_AWAIT rpc::make_optimistic(tmp);
                    CO_RETURN result;
                }
                else
                {
                    result.iface = tmp;
                    CO_RETURN result;
                }
            }
        }
    };

}
