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
        static constexpr uint32_t staging_buffer_wait_attempt_limit = 4'000'000;

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

    // Staging buffer pool overview
    // ----------------------------
    //
    // The io_uring descriptor exposes one host-visible byte region:
    //
    //     buffer_region_ptr
    //          |
    //          v
    //     +---------+---------+---------+---------+
    //     | slot 0  | slot 1  | slot 2  | slot 3  | ...
    //     +---------+---------+---------+---------+
    //         ^         ^
    //         |         |
    //     staging_buffer shared_ptr guards mark these slots in use
    //
    // Every staged direct operation that needs the kernel to read or write data
    // borrows one of these slots. The slot is released when the last
    // shared_ptr<staging_buffer> for that operation is destroyed. Operations
    // that need two kernel-visible pointers reserve both slots as one unit.
    //
    // Under pressure, allocation is FIFO:
    //
    //     allocate_staging_buffers()
    //          |
    //          +-- immediate reserve if no one is waiting
    //          |
    //          +-- otherwise enqueue waiter and suspend
    //                                      |
    //     release_staging_buffer() -------+
    //          |
    //          +-- grant front waiter, then resume it outside the spin lock

    // Records one borrowed slot from the ring-visible staging buffer region.
    // The object is deliberately small because destruction returns the slot.
    controller::staging_buffer::staging_buffer(
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

    // Releases the borrowed staging buffer slot back to the controller cache when
    // the operation-specific shared_ptr is destroyed.
    controller::staging_buffer::~staging_buffer()
    {
        if (controller_)
        {
            controller_->release_staging_buffer(slot_, generation_);
        }
    }

    // Returns the ring-visible byte address used for memcpy before/after an
    // io_uring operation.
    uint8_t* controller::staging_buffer::data() const noexcept
    {
        return data_;
    }

    // Returns the usable size of this slot, which may be smaller than the slot
    // size if the caller requested fewer bytes.
    size_t controller::staging_buffer::size() const noexcept
    {
        return size_;
    }

    // Returns the raw address that is written into SQEs for kernel I/O. This is
    // ring-visible memory and must never be treated as trusted storage.
    uint64_t controller::staging_buffer::address() const noexcept
    {
        return static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(data_));
    }

    // Builds or refreshes the local view of the staging buffer pool. The
    // descriptor gives one contiguous ring-visible region; this function
    // validates the region, sizes the slot bitmap, and bumps the generation so
    // old staging_buffer destructors cannot release slots after a remap.
    int controller::initialize_staging_buffer_cache_locked(const data& ring_data)
    {
        const auto& buffers = ring_data.buffers;
        if (buffer_region_ptr_ == buffers.buffer_region_ptr && staging_buffer_count_ == buffers.buffer_count
            && staging_buffer_size_ == buffers.buffer_size && !staging_buffer_slots_in_use_.empty())
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
            staging_buffer_slots_in_use_.assign(buffers.buffer_count, 0);
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while sizing direct io_uring staging buffer slots");
            std::terminate();
        }

        buffer_region_ptr_ = buffers.buffer_region_ptr;
        staging_buffer_count_ = buffers.buffer_count;
        staging_buffer_size_ = buffers.buffer_size;
        ++staging_buffer_generation_;
        return rpc::error::OK();
    }

    std::shared_ptr<controller::staging_buffer_waiter> controller::make_staging_buffer_waiter(
        uint32_t required_buffer_count,
        size_t first_requested_size,
        size_t second_requested_size,
        int& error_code) const
    {
        try
        {
            auto waiter = std::make_shared<staging_buffer_waiter>();
            waiter->required_buffer_count = required_buffer_count;
            waiter->first_requested_size = first_requested_size;
            waiter->second_requested_size = second_requested_size;
            error_code = rpc::error::OK();
            return waiter;
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating direct io_uring staging buffer waiter");
            std::terminate();
        }

        return {};
    }

    struct controller::staging_buffer_wait_awaiter
    {
        controller& controller;
        std::shared_ptr<staging_buffer_waiter> waiter;

        // If release_staging_buffer() granted the waiter before the coroutine
        // reached co_await, do not suspend. This is the first half of the
        // lost-wakeup protection.
        bool await_ready() const noexcept
        {
            return !waiter || waiter->granted || waiter->error_code != rpc::error::OK();
        }

        // Store the coroutine handle only while holding the same mutex used by
        // release_staging_buffer(). If the waiter is granted or failed between
        // await_ready() and await_suspend(), returning false resumes the
        // coroutine immediately instead of leaving it asleep forever.
        bool await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept
        {
            std::lock_guard<rpc::spin_mutex> lock(controller.staging_buffer_mutex_);
            if (!waiter || waiter->granted || waiter->error_code != rpc::error::OK() || !waiter->queued)
            {
                return false;
            }

            waiter->waiter = awaiting_coroutine;
            return true;
        }

        void await_resume() const noexcept { }
    };

    // Reserves one or two slots while staging_buffer_mutex_ is held. No
    // staging_buffer objects are constructed here, so memory allocation never
    // happens inside the spin-lock critical section.
    int controller::reserve_staging_buffers_locked(
        uint32_t required_buffer_count,
        size_t first_requested_size,
        size_t second_requested_size,
        staging_buffer_reservation& first_reservation,
        staging_buffer_reservation& second_reservation) noexcept
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

        if (staging_buffer_count_ < required_buffer_count)
        {
            return rpc::error::RESOURCE_EXHAUSTED();
        }

        // First find all required slots, then mark them in use. This all-or-none
        // reservation is important for two-buffer operations because a partial
        // reservation would let a coroutine hold a scarce slot while waiting for
        // another one.
        uint32_t slots[2]{std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()};
        uint32_t found_count = 0;
        for (uint32_t slot = 0; slot < staging_buffer_count_ && found_count < required_buffer_count; ++slot)
        {
            if (staging_buffer_slots_in_use_[slot] != 0)
            {
                continue;
            }

            slots[found_count++] = slot;
        }

        if (found_count != required_buffer_count)
        {
            return rpc::error::RESOURCE_EXHAUSTED();
        }

        const auto first_offset = static_cast<uint64_t>(slots[0]) * staging_buffer_size_;
        if (first_offset > std::numeric_limits<uint64_t>::max() - buffer_region_ptr_)
        {
            return rpc::error::PROTOCOL_ERROR();
        }

        first_reservation = {slots[0],
            staging_buffer_generation_,
            detail::ring_pointer<uint8_t>(buffer_region_ptr_ + first_offset),
            std::min(first_requested_size, static_cast<size_t>(staging_buffer_size_))};

        if (required_buffer_count == 2)
        {
            const auto second_offset = static_cast<uint64_t>(slots[1]) * staging_buffer_size_;
            if (second_offset > std::numeric_limits<uint64_t>::max() - buffer_region_ptr_)
            {
                return rpc::error::PROTOCOL_ERROR();
            }

            second_reservation = {slots[1],
                staging_buffer_generation_,
                detail::ring_pointer<uint8_t>(buffer_region_ptr_ + second_offset),
                std::min(second_requested_size, static_cast<size_t>(staging_buffer_size_))};
        }

        for (uint32_t index = 0; index < required_buffer_count; ++index)
        {
            staging_buffer_slots_in_use_[slots[index]] = 1;
        }

        return rpc::error::OK();
    }

    std::shared_ptr<controller::staging_buffer_waiter> controller::grant_next_staging_buffer_waiter_locked() noexcept
    {
        auto shutdown_err = shutdown_error();
        if (shutdown_err != rpc::error::OK())
        {
            for (auto& waiter : staging_buffer_waiters_)
            {
                if (waiter)
                {
                    waiter->error_code = shutdown_err;
                    waiter->queued = false;
                }
            }
            staging_buffer_waiters_.clear();
            return {};
        }

        while (!staging_buffer_waiters_.empty())
        {
            auto waiter = staging_buffer_waiters_.front();
            if (!waiter)
            {
                staging_buffer_waiters_.pop_front();
                continue;
            }

            waiter->error_code = reserve_staging_buffers_locked(
                waiter->required_buffer_count,
                waiter->first_requested_size,
                waiter->second_requested_size,
                waiter->first_reservation,
                waiter->second_reservation);
            if (waiter->error_code == rpc::error::RESOURCE_EXHAUSTED())
            {
                // Exhaustion is ordinary backpressure, not a failed waiter.
                // Leave the waiter at the front so it keeps its FIFO position
                // and retry when another staging_buffer guard is released.
                waiter->error_code = rpc::error::OK();
                return {};
            }

            staging_buffer_waiters_.pop_front();
            waiter->granted = true;
            waiter->queued = false;
            return waiter;
        }

        return {};
    }

    void controller::cancel_staging_buffer_waiter(const std::shared_ptr<staging_buffer_waiter>& waiter) noexcept
    {
        if (!waiter)
        {
            return;
        }

        uint32_t reserved_count = 0;
        staging_buffer_reservation first_reservation;
        staging_buffer_reservation second_reservation;
        {
            std::lock_guard<rpc::spin_mutex> lock(staging_buffer_mutex_);
            auto found = std::find(staging_buffer_waiters_.begin(), staging_buffer_waiters_.end(), waiter);
            if (found != staging_buffer_waiters_.end())
            {
                staging_buffer_waiters_.erase(found);
            }

            if (waiter->granted)
            {
                // The waiter may have been granted while the coroutine was
                // being cancelled. Release those reserved slots so the next
                // queued waiter can make progress.
                reserved_count = waiter->required_buffer_count;
                first_reservation = waiter->first_reservation;
                second_reservation = waiter->second_reservation;
                waiter->granted = false;
            }
            waiter->queued = false;
            waiter->waiter = {};
        }

        if (reserved_count >= 1)
        {
            release_staging_buffer(first_reservation.slot, first_reservation.generation);
        }
        if (reserved_count >= 2)
        {
            release_staging_buffer(second_reservation.slot, second_reservation.generation);
        }
    }

    void controller::fail_staging_buffer_waiters(int error_code) noexcept
    {
        std::deque<std::shared_ptr<staging_buffer_waiter>> failed_waiters;
        {
            std::lock_guard<rpc::spin_mutex> lock(staging_buffer_mutex_);
            for (auto& waiter : staging_buffer_waiters_)
            {
                if (waiter)
                {
                    waiter->error_code = error_code;
                    waiter->queued = false;
                    waiter->granted = false;
                }
            }
            staging_buffer_waiters_.swap(failed_waiters);
        }

        // Resuming while holding staging_buffer_mutex_ would let user coroutine
        // code re-enter allocation or release paths from inside the lock.
        for (const auto& waiter : failed_waiters)
        {
            resume_staging_buffer_waiter(waiter);
        }
    }

    void controller::resume_staging_buffer_waiter(const std::shared_ptr<staging_buffer_waiter>& waiter) noexcept
    {
        if (!waiter)
        {
            return;
        }

        auto awaiting_coroutine = waiter->waiter;
        waiter->waiter = {};
        if (!awaiting_coroutine || awaiting_coroutine.done())
        {
            return;
        }

        // Use the controller scheduler when available so a buffer release does
        // not run arbitrary coroutine work inline on the releasing call stack.
        if (scheduler_)
        {
            scheduler_->resume(awaiting_coroutine);
        }
        else
        {
            awaiting_coroutine.resume();
        }
    }

    controller::staging_buffer_pair_allocation_result controller::make_staging_buffers_from_reservations(
        uint32_t required_buffer_count,
        const staging_buffer_reservation& first_reservation,
        const staging_buffer_reservation& second_reservation)
    {
        if (required_buffer_count == 0 || required_buffer_count > 2)
        {
            return {rpc::error::INVALID_DATA(), {}, {}};
        }

        std::unique_ptr<staging_buffer> first_guard;
        try
        {
            first_guard = std::unique_ptr<staging_buffer>(new staging_buffer(
                this, first_reservation.slot, first_reservation.generation, first_reservation.data, first_reservation.size));
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating direct io_uring staging buffer");
            std::terminate();
        }

        std::unique_ptr<staging_buffer> second_guard;
        if (required_buffer_count == 2)
        {
            try
            {
                second_guard = std::unique_ptr<staging_buffer>(new staging_buffer(
                    this,
                    second_reservation.slot,
                    second_reservation.generation,
                    second_reservation.data,
                    second_reservation.size));
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while creating second direct io_uring staging buffer");
                std::terminate();
            }
        }

        std::shared_ptr<staging_buffer> first_buffer;
        try
        {
            first_buffer = std::shared_ptr<staging_buffer>(std::move(first_guard));
        }
        catch (const std::bad_alloc&)
        {
            RPC_ERROR("bad_alloc while creating direct io_uring staging buffer shared pointer");
            std::terminate();
        }

        std::shared_ptr<staging_buffer> second_buffer;
        if (required_buffer_count == 2)
        {
            try
            {
                second_buffer = std::shared_ptr<staging_buffer>(std::move(second_guard));
            }
            catch (const std::bad_alloc&)
            {
                RPC_ERROR("bad_alloc while creating second direct io_uring staging buffer shared pointer");
                std::terminate();
            }
        }

        return {rpc::error::OK(), std::move(first_buffer), std::move(second_buffer)};
    }

    // Common staging-buffer admission path. Under pressure, waiters join a FIFO
    // queue and only the head waiter is allowed to reserve the next available
    // slots. The waiter still yields through the scheduler so allocation stays
    // bounded by staging_buffer_wait_attempt_limit.
    CORO_TASK(controller::staging_buffer_pair_allocation_result)
    controller::allocate_staging_buffers(
        uint32_t required_buffer_count,
        size_t first_requested_size,
        size_t second_requested_size)
    {
        if (required_buffer_count == 0 || required_buffer_count > 2 || first_requested_size == 0
            || (required_buffer_count == 2 && second_requested_size == 0))
        {
            CO_RETURN staging_buffer_pair_allocation_result{rpc::error::INVALID_DATA(), {}, {}};
        }

        auto err = CO_AWAIT ensure_iouring_data();
        if (err != rpc::error::OK())
        {
            CO_RETURN staging_buffer_pair_allocation_result{err, {}, {}};
        }

        std::shared_ptr<staging_buffer_waiter> waiter;
        for (uint32_t attempt = 0; attempt < staging_buffer_wait_attempt_limit; ++attempt)
        {
            auto shutdown_err = shutdown_error();
            if (shutdown_err != rpc::error::OK())
            {
                cancel_staging_buffer_waiter(waiter);
                CO_RETURN staging_buffer_pair_allocation_result{shutdown_err, {}, {}};
            }

            const auto ring_data = cached_iouring_data_copy();
            staging_buffer_reservation first_reservation;
            staging_buffer_reservation second_reservation;
            bool reserved = false;
            bool needs_waiter = false;
            bool should_wait = false;

            {
                std::lock_guard<rpc::spin_mutex> lock(staging_buffer_mutex_);
                err = shutdown_error();
                if (err != rpc::error::OK())
                {
                    CO_RETURN staging_buffer_pair_allocation_result{err, {}, {}};
                }

                err = initialize_staging_buffer_cache_locked(ring_data);
                if (err != rpc::error::OK())
                {
                    CO_RETURN staging_buffer_pair_allocation_result{err, {}, {}};
                }

                if (waiter && waiter->granted)
                {
                    if (waiter->error_code != rpc::error::OK())
                    {
                        CO_RETURN staging_buffer_pair_allocation_result{waiter->error_code, {}, {}};
                    }

                    first_reservation = waiter->first_reservation;
                    second_reservation = waiter->second_reservation;
                    reserved = true;
                }
                else if (!waiter || !waiter->queued)
                {
                    if (staging_buffer_waiters_.empty())
                    {
                        // Fast path: no queued waiters means this coroutine can
                        // try the pool directly. Once anyone is queued, later
                        // callers must join the queue to avoid cutting ahead.
                        err = reserve_staging_buffers_locked(
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
                            CO_RETURN staging_buffer_pair_allocation_result{err, {}, {}};
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
                                staging_buffer_waiters_.push_back(waiter);
                                waiter->queued = true;
                            }
                            catch (const std::bad_alloc&)
                            {
                                RPC_ERROR("bad_alloc while queuing direct io_uring staging buffer waiter");
                                std::terminate();
                            }

                            // There may already be enough free slots by the
                            // time the waiter is enqueued. Grant while still
                            // under the mutex so the waiter cannot miss the
                            // transition from queued to granted.
                            grant_next_staging_buffer_waiter_locked();
                            if (waiter->granted)
                            {
                                if (waiter->error_code != rpc::error::OK())
                                {
                                    CO_RETURN staging_buffer_pair_allocation_result{waiter->error_code, {}, {}};
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
                CO_RETURN make_staging_buffers_from_reservations(
                    required_buffer_count, first_reservation, second_reservation);
            }

            if (needs_waiter)
            {
                waiter
                    = make_staging_buffer_waiter(required_buffer_count, first_requested_size, second_requested_size, err);
                if (!waiter)
                {
                    CO_RETURN staging_buffer_pair_allocation_result{err, {}, {}};
                }
                continue;
            }

            if (should_wait)
            {
                // Suspends until release_staging_buffer() reserves the slots
                // for this exact waiter or shutdown fails the queue.
                CO_AWAIT staging_buffer_wait_awaiter{*this, waiter};
            }
            else
            {
                CO_AWAIT wait_before_next_poll();
            }
        }

        cancel_staging_buffer_waiter(waiter);
        CO_RETURN staging_buffer_pair_allocation_result{rpc::error::RESOURCE_EXHAUSTED(), {}, {}};
    }

    // Borrows one slot from the registered staging buffer pool for a single SQE.
    // The returned shared_ptr keeps the slot reserved until the submission and
    // completion path is finished with the ring-visible memory.
    CORO_TASK(controller::staging_buffer_allocation_result)
    controller::allocate_staging_buffer(size_t requested_size)
    {
        auto result = CO_AWAIT allocate_staging_buffers(1, requested_size, 0);
        CO_RETURN staging_buffer_allocation_result{result.error_code, std::move(result.first_buffer)};
    }

    // Borrows two staging buffer slots as one admission decision. Linked
    // operations such as RECV+LINK_TIMEOUT need both buffers before any SQE is
    // submitted; reserving them together avoids holding one scarce slot while
    // waiting for another.
    CORO_TASK(controller::staging_buffer_pair_allocation_result)
    controller::allocate_staging_buffer_pair(
        size_t first_requested_size,
        size_t second_requested_size)
    {
        CO_RETURN CO_AWAIT allocate_staging_buffers(2, first_requested_size, second_requested_size);
    }

    // Marks a slot as free when the matching staging_buffer guard is destroyed.
    // The generation check protects against stale guards from a previous
    // staging buffer mapping.
    void controller::release_staging_buffer(
        uint32_t slot,
        uint64_t generation) noexcept
    {
        std::shared_ptr<staging_buffer_waiter> waiter_to_resume;
        {
            std::lock_guard<rpc::spin_mutex> lock(staging_buffer_mutex_);
            if (generation != staging_buffer_generation_ || slot >= staging_buffer_slots_in_use_.size())
            {
                return;
            }

            staging_buffer_slots_in_use_[slot] = 0;
            waiter_to_resume = grant_next_staging_buffer_waiter_locked();
        }
        // The waiter now owns any granted slots. Resume it after dropping the
        // lock so its coroutine can allocate guards or release on failure
        // without recursively entering the staging-buffer mutex.
        resume_staging_buffer_waiter(waiter_to_resume);
    }

    // Allocates a staging buffer and writes an IPv4 sockaddr into it so
    // bind/connect SQEs can pass a kernel-readable address pointer.
    CORO_TASK(controller::staging_buffer_allocation_result)
    controller::make_ipv4_address_buffer(
        const std::array<
            uint8_t,
            4>& bind_address,
        uint16_t port)
    {
        auto allocation = CO_AWAIT allocate_staging_buffer(sizeof(direct_ipv4_sockaddr));
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

    CORO_TASK(controller::staging_buffer_allocation_result)
    controller::make_loopback_address_buffer(uint16_t port)
    {
        CO_RETURN CO_AWAIT make_ipv4_address_buffer({127, 0, 0, 1}, port);
    }

    CORO_TASK(controller::staging_buffer_allocation_result)
    controller::make_ipv6_address_buffer(
        const std::array<
            uint8_t,
            16>& bind_address,
        uint16_t port)
    {
        auto allocation = CO_AWAIT allocate_staging_buffer(sizeof(direct_ipv6_sockaddr));
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
