/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <io_uring/controller.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace rpc::io_uring
{
    namespace
    {
        static constexpr int32_t socket_family_inet = 2;
        static constexpr int32_t socket_family_inet6 = 10;
        static constexpr size_t ipv4_sockaddr_size = 16;
        static constexpr size_t ipv6_sockaddr_size = 28;
        static constexpr uint32_t host_buffer_wait_attempt_limit = 4'000'000;

        struct direct_ipv4_sockaddr
        {
            uint16_t family{socket_family_inet};
            uint16_t port_be{0};
            uint8_t address[4]{127, 0, 0, 1};
            uint8_t zero[8]{};
        };

        static_assert(sizeof(direct_ipv4_sockaddr) == ipv4_sockaddr_size);

        struct direct_ipv6_sockaddr
        {
            uint16_t family{socket_family_inet6};
            uint16_t port_be{0};
            uint32_t flowinfo_be{0};
            uint8_t address[16]{};
            uint32_t scope_id_be{0};
        };

        static_assert(sizeof(direct_ipv6_sockaddr) == ipv6_sockaddr_size);

        // Converts the test loopback port into the network byte order expected
        // by the kernel sockaddr structure.
        uint16_t host_to_network_u16(uint16_t value) noexcept
        {
            return static_cast<uint16_t>((value >> 8) | (value << 8));
        }
    } // namespace

    // Records one borrowed slot from the host-registered buffer region. The
    // object is deliberately small because destruction returns the slot.
    controller::host_buffer::host_buffer(
        controller* controller,
        uint32_t slot,
        uint64_t generation,
        uint8_t* data,
        size_t size) noexcept
        : controller_(controller)
        , slot_(slot)
        , generation_(generation)
        , data_(data)
        , size_(size)
    {
    }

    // Releases the borrowed host buffer slot back to the controller cache when
    // the operation-specific shared_ptr is destroyed.
    controller::host_buffer::~host_buffer()
    {
        if (controller_)
        {
            controller_->release_host_buffer(slot_, generation_);
        }
    }

    // Returns the host-visible byte address used for memcpy before/after an
    // io_uring operation.
    uint8_t* controller::host_buffer::data() const noexcept
    {
        return data_;
    }

    // Returns the usable size of this slot, which may be smaller than the slot
    // size if the caller requested fewer bytes.
    size_t controller::host_buffer::size() const noexcept
    {
        return size_;
    }

    // Returns the raw address that is written into SQEs for kernel I/O. This is
    // outside-enclave memory and must never be treated as trusted storage.
    uint64_t controller::host_buffer::address() const noexcept
    {
        return static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(data_));
    }

    // Builds or refreshes the local view of the host-registered buffer pool.
    // The descriptor gives one contiguous outside-enclave region; this function
    // validates the region, sizes the slot bitmap, and bumps the generation so
    // old host_buffer destructors cannot release slots after a remap.
    int controller::initialize_host_buffer_cache_locked(const data& ring_data)
    {
        const auto& buffers = ring_data.buffers;
        if (buffer_region_ptr_ == buffers.buffer_region_ptr && host_buffer_count_ == buffers.buffer_count
            && host_buffer_size_ == buffers.buffer_size && !host_buffer_slots_in_use_.empty())
        {
            return rpc::error::OK();
        }

        if (buffers.buffer_region_ptr == 0 || buffers.buffer_count == 0 || buffers.buffer_size == 0)
        {
            return rpc::error::PROTOCOL_ERROR();
        }

        size_t expected_region_size = 0;
        if (!detail::checked_size(buffers.buffer_count, buffers.buffer_size, expected_region_size)
            || expected_region_size > buffers.buffer_region_size)
        {
            return rpc::error::PROTOCOL_ERROR();
        }

        if (!detail::is_accessible_ring_pointer(buffers.buffer_region_ptr, expected_region_size))
        {
            return rpc::error::PROTOCOL_ERROR();
        }

        try
        {
            host_buffer_slots_in_use_.assign(buffers.buffer_count, 0);
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while sizing direct io_uring host buffer slots");
            std::terminate();
        }

        buffer_region_ptr_ = buffers.buffer_region_ptr;
        host_buffer_count_ = buffers.buffer_count;
        host_buffer_size_ = buffers.buffer_size;
        ++host_buffer_generation_;
        return rpc::error::OK();
    }

    std::shared_ptr<controller::host_buffer_waiter> controller::make_host_buffer_waiter(
        uint32_t required_buffer_count,
        size_t first_requested_size,
        size_t second_requested_size,
        int& error_code) const
    {
        try
        {
            auto waiter = std::make_shared<host_buffer_waiter>();
            waiter->required_buffer_count = required_buffer_count;
            waiter->first_requested_size = first_requested_size;
            waiter->second_requested_size = second_requested_size;
            error_code = rpc::error::OK();
            return waiter;
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating direct io_uring host buffer waiter");
            std::terminate();
        }

        return {};
    }

    // Reserves one or two slots while host_buffer_mutex_ is held. No
    // host_buffer objects are constructed here, so memory allocation never
    // happens inside the spin-lock critical section.
    int controller::reserve_host_buffers_locked(
        uint32_t required_buffer_count,
        size_t first_requested_size,
        size_t second_requested_size,
        host_buffer_reservation& first_reservation,
        host_buffer_reservation& second_reservation) noexcept
    {
        first_reservation = {};
        second_reservation = {};

        if (required_buffer_count == 0 || required_buffer_count > 2 || first_requested_size == 0
            || (required_buffer_count == 2 && second_requested_size == 0))
        {
            return rpc::error::INVALID_DATA();
        }

        auto shutdown_err = shutdown_error();
        if (shutdown_err != rpc::error::OK())
        {
            return shutdown_err;
        }

        if (host_buffer_count_ < required_buffer_count)
        {
            return rpc::error::RESOURCE_EXHAUSTED();
        }

        uint32_t slots[2]{std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()};
        uint32_t found_count = 0;
        for (uint32_t slot = 0; slot < host_buffer_count_ && found_count < required_buffer_count; ++slot)
        {
            if (host_buffer_slots_in_use_[slot] != 0)
            {
                continue;
            }

            slots[found_count++] = slot;
        }

        if (found_count != required_buffer_count)
        {
            return rpc::error::RESOURCE_EXHAUSTED();
        }

        const auto first_offset = static_cast<uint64_t>(slots[0]) * host_buffer_size_;
        if (first_offset > std::numeric_limits<uint64_t>::max() - buffer_region_ptr_)
        {
            return rpc::error::PROTOCOL_ERROR();
        }

        first_reservation = {slots[0],
            host_buffer_generation_,
            detail::ring_pointer<uint8_t>(buffer_region_ptr_ + first_offset),
            std::min(first_requested_size, static_cast<size_t>(host_buffer_size_))};

        if (required_buffer_count == 2)
        {
            const auto second_offset = static_cast<uint64_t>(slots[1]) * host_buffer_size_;
            if (second_offset > std::numeric_limits<uint64_t>::max() - buffer_region_ptr_)
            {
                return rpc::error::PROTOCOL_ERROR();
            }

            second_reservation = {slots[1],
                host_buffer_generation_,
                detail::ring_pointer<uint8_t>(buffer_region_ptr_ + second_offset),
                std::min(second_requested_size, static_cast<size_t>(host_buffer_size_))};
        }

        for (uint32_t index = 0; index < required_buffer_count; ++index)
        {
            host_buffer_slots_in_use_[slots[index]] = 1;
        }

        return rpc::error::OK();
    }

    std::shared_ptr<controller::host_buffer_waiter> controller::grant_next_host_buffer_waiter_locked() noexcept
    {
        auto shutdown_err = shutdown_error();
        if (shutdown_err != rpc::error::OK())
        {
            for (auto& waiter : host_buffer_waiters_)
            {
                if (waiter)
                {
                    waiter->error_code = shutdown_err;
                    waiter->queued = false;
                }
            }
            host_buffer_waiters_.clear();
            return {};
        }

        while (!host_buffer_waiters_.empty())
        {
            auto waiter = host_buffer_waiters_.front();
            if (!waiter)
            {
                host_buffer_waiters_.pop_front();
                continue;
            }

            waiter->error_code = reserve_host_buffers_locked(
                waiter->required_buffer_count,
                waiter->first_requested_size,
                waiter->second_requested_size,
                waiter->first_reservation,
                waiter->second_reservation);
            if (waiter->error_code == rpc::error::RESOURCE_EXHAUSTED())
            {
                return {};
            }

            host_buffer_waiters_.pop_front();
            waiter->granted = true;
            waiter->queued = false;
            return waiter;
        }

        return {};
    }

    void controller::cancel_host_buffer_waiter(const std::shared_ptr<host_buffer_waiter>& waiter) noexcept
    {
        if (!waiter)
        {
            return;
        }

        uint32_t reserved_count = 0;
        host_buffer_reservation first_reservation;
        host_buffer_reservation second_reservation;
        {
            std::lock_guard<rpc::spin_mutex> lock(host_buffer_mutex_);
            auto found = std::find(host_buffer_waiters_.begin(), host_buffer_waiters_.end(), waiter);
            if (found != host_buffer_waiters_.end())
            {
                host_buffer_waiters_.erase(found);
            }

            if (waiter->granted)
            {
                reserved_count = waiter->required_buffer_count;
                first_reservation = waiter->first_reservation;
                second_reservation = waiter->second_reservation;
                waiter->granted = false;
            }
            waiter->queued = false;
        }

        if (reserved_count >= 1)
        {
            release_host_buffer(first_reservation.slot, first_reservation.generation);
        }
        if (reserved_count >= 2)
        {
            release_host_buffer(second_reservation.slot, second_reservation.generation);
        }
    }

    void controller::fail_host_buffer_waiters(int error_code) noexcept
    {
        std::lock_guard<rpc::spin_mutex> lock(host_buffer_mutex_);
        for (auto& waiter : host_buffer_waiters_)
        {
            if (waiter)
            {
                waiter->error_code = error_code;
                waiter->queued = false;
                waiter->granted = false;
            }
        }
        host_buffer_waiters_.clear();
    }

    controller::host_buffer_pair_allocation_result controller::make_host_buffers_from_reservations(
        uint32_t required_buffer_count,
        const host_buffer_reservation& first_reservation,
        const host_buffer_reservation& second_reservation)
    {
        if (required_buffer_count == 0 || required_buffer_count > 2)
        {
            return {rpc::error::INVALID_DATA(), {}, {}};
        }

        std::unique_ptr<host_buffer> first_guard;
        try
        {
            first_guard = std::unique_ptr<host_buffer>(new host_buffer(
                this, first_reservation.slot, first_reservation.generation, first_reservation.data, first_reservation.size));
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating direct io_uring host buffer");
            std::terminate();
        }

        std::unique_ptr<host_buffer> second_guard;
        if (required_buffer_count == 2)
        {
            try
            {
                second_guard = std::unique_ptr<host_buffer>(new host_buffer(
                    this,
                    second_reservation.slot,
                    second_reservation.generation,
                    second_reservation.data,
                    second_reservation.size));
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while creating second direct io_uring host buffer");
                std::terminate();
            }
        }

        std::shared_ptr<host_buffer> first_buffer;
        try
        {
            first_buffer = std::shared_ptr<host_buffer>(std::move(first_guard));
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating direct io_uring host buffer shared pointer");
            std::terminate();
        }

        std::shared_ptr<host_buffer> second_buffer;
        if (required_buffer_count == 2)
        {
            try
            {
                second_buffer = std::shared_ptr<host_buffer>(std::move(second_guard));
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while creating second direct io_uring host buffer shared pointer");
                std::terminate();
            }
        }

        return {rpc::error::OK(), std::move(first_buffer), std::move(second_buffer)};
    }

    // Common host-buffer admission path. Under pressure, waiters join a FIFO
    // queue and only the head waiter is allowed to reserve the next available
    // slots. The waiter still yields through the scheduler so allocation stays
    // bounded by host_buffer_wait_attempt_limit.
    CORO_TASK(controller::host_buffer_pair_allocation_result)
    controller::allocate_host_buffers(
        uint32_t required_buffer_count,
        size_t first_requested_size,
        size_t second_requested_size)
    {
        if (required_buffer_count == 0 || required_buffer_count > 2 || first_requested_size == 0
            || (required_buffer_count == 2 && second_requested_size == 0))
        {
            CO_RETURN host_buffer_pair_allocation_result{rpc::error::INVALID_DATA(), {}, {}};
        }

        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN host_buffer_pair_allocation_result{err, {}, {}};
        }

        std::shared_ptr<host_buffer_waiter> waiter;
        for (uint32_t attempt = 0; attempt < host_buffer_wait_attempt_limit; ++attempt)
        {
            auto shutdown_err = shutdown_error();
            if (shutdown_err != rpc::error::OK())
            {
                cancel_host_buffer_waiter(waiter);
                CO_RETURN host_buffer_pair_allocation_result{shutdown_err, {}, {}};
            }

            const auto ring_data = cached_iouring_data_copy();
            host_buffer_reservation first_reservation;
            host_buffer_reservation second_reservation;
            bool reserved = false;
            bool needs_waiter = false;
            bool should_wait = false;

            {
                std::lock_guard<rpc::spin_mutex> lock(host_buffer_mutex_);
                err = shutdown_error();
                if (err != rpc::error::OK())
                {
                    CO_RETURN host_buffer_pair_allocation_result{err, {}, {}};
                }

                err = initialize_host_buffer_cache_locked(ring_data);
                if (err != rpc::error::OK())
                {
                    CO_RETURN host_buffer_pair_allocation_result{err, {}, {}};
                }

                if (waiter && waiter->granted)
                {
                    if (waiter->error_code != rpc::error::OK())
                    {
                        CO_RETURN host_buffer_pair_allocation_result{waiter->error_code, {}, {}};
                    }

                    first_reservation = waiter->first_reservation;
                    second_reservation = waiter->second_reservation;
                    reserved = true;
                }
                else if (!waiter || !waiter->queued)
                {
                    if (host_buffer_waiters_.empty())
                    {
                        err = reserve_host_buffers_locked(
                            required_buffer_count,
                            first_requested_size,
                            second_requested_size,
                            first_reservation,
                            second_reservation);
                        if (err == rpc::error::OK())
                        {
                            reserved = true;
                        }
                        else if (err != rpc::error::RESOURCE_EXHAUSTED())
                        {
                            CO_RETURN host_buffer_pair_allocation_result{err, {}, {}};
                        }
                    }

                    if (!reserved)
                    {
                        if (!waiter)
                        {
                            needs_waiter = true;
                        }
                        else
                        {
                            try
                            {
                                host_buffer_waiters_.push_back(waiter);
                                waiter->queued = true;
                            }
                            catch (const std::bad_alloc&)
                            {
                                RPC_ERROR("bad_alloc while queuing direct io_uring host buffer waiter");
                                std::terminate();
                            }

                            grant_next_host_buffer_waiter_locked();
                            if (waiter->granted)
                            {
                                if (waiter->error_code != rpc::error::OK())
                                {
                                    CO_RETURN host_buffer_pair_allocation_result{waiter->error_code, {}, {}};
                                }

                                first_reservation = waiter->first_reservation;
                                second_reservation = waiter->second_reservation;
                                reserved = true;
                            }
                            else
                            {
                                should_wait = true;
                            }
                        }
                    }
                }
                else
                {
                    should_wait = true;
                }
            }

            if (reserved)
            {
                CO_RETURN make_host_buffers_from_reservations(required_buffer_count, first_reservation, second_reservation);
            }

            if (needs_waiter)
            {
                waiter = make_host_buffer_waiter(required_buffer_count, first_requested_size, second_requested_size, err);
                if (!waiter)
                {
                    CO_RETURN host_buffer_pair_allocation_result{err, {}, {}};
                }
                continue;
            }

            (void)should_wait;
            CO_AWAIT wait_before_next_poll();
        }

        cancel_host_buffer_waiter(waiter);
        CO_RETURN host_buffer_pair_allocation_result{rpc::error::RESOURCE_EXHAUSTED(), {}, {}};
    }

    // Borrows one slot from the registered host buffer pool for a single SQE.
    // The returned shared_ptr keeps the slot reserved until the submission and
    // completion path is finished with the host memory.
    CORO_TASK(controller::host_buffer_allocation_result)
    controller::allocate_host_buffer(size_t requested_size)
    {
        auto result = CO_AWAIT allocate_host_buffers(1, requested_size, 0);
        CO_RETURN host_buffer_allocation_result{result.error_code, std::move(result.first_buffer)};
    }

    // Borrows two host buffer slots as one admission decision. Linked
    // operations such as RECV+LINK_TIMEOUT need both buffers before any SQE is
    // submitted; reserving them together avoids holding one scarce slot while
    // waiting for another.
    CORO_TASK(controller::host_buffer_pair_allocation_result)
    controller::allocate_host_buffer_pair(
        size_t first_requested_size,
        size_t second_requested_size)
    {
        CO_RETURN CO_AWAIT allocate_host_buffers(2, first_requested_size, second_requested_size);
    }

    // Marks a slot as free when the matching host_buffer guard is destroyed.
    // The generation check protects against stale guards from a previous host
    // buffer mapping.
    void controller::release_host_buffer(
        uint32_t slot,
        uint64_t generation) noexcept
    {
        std::lock_guard<rpc::spin_mutex> lock(host_buffer_mutex_);
        if (generation != host_buffer_generation_ || slot >= host_buffer_slots_in_use_.size())
        {
            return;
        }

        host_buffer_slots_in_use_[slot] = 0;
        grant_next_host_buffer_waiter_locked();
    }

    // Allocates a host buffer and writes an IPv4 loopback sockaddr into it so
    // bind/connect SQEs can pass a kernel-readable address pointer.
    CORO_TASK(controller::host_buffer_allocation_result)
    controller::make_ipv4_address_buffer(
        const std::array<uint8_t, 4>& bind_address,
        uint16_t port)
    {
        auto allocation = CO_AWAIT allocate_host_buffer(sizeof(direct_ipv4_sockaddr));
        if (allocation.error_code != rpc::error::OK())
        {
            CO_RETURN allocation;
        }

        direct_ipv4_sockaddr address{};
        address.port_be = host_to_network_u16(port);
        std::memcpy(address.address, bind_address.data(), bind_address.size());
        std::memcpy(allocation.buffer->data(), &address, sizeof(address));
        CO_RETURN allocation;
    }

    CORO_TASK(controller::host_buffer_allocation_result)
    controller::make_loopback_address_buffer(uint16_t port)
    {
        CO_RETURN CO_AWAIT make_ipv4_address_buffer({127, 0, 0, 1}, port);
    }

    CORO_TASK(controller::host_buffer_allocation_result)
    controller::make_ipv6_address_buffer(
        const std::array<uint8_t, 16>& bind_address,
        uint16_t port)
    {
        auto allocation = CO_AWAIT allocate_host_buffer(sizeof(direct_ipv6_sockaddr));
        if (allocation.error_code != rpc::error::OK())
        {
            CO_RETURN allocation;
        }

        direct_ipv6_sockaddr address{};
        address.port_be = host_to_network_u16(port);
        std::memcpy(address.address, bind_address.data(), bind_address.size());
        std::memcpy(allocation.buffer->data(), &address, sizeof(address));
        CO_RETURN allocation;
    }
} // namespace rpc::io_uring
