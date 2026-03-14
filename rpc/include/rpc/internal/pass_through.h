/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

/**
 * @file pass_through.h
 * @brief Routes RPC calls through intermediary zones to reach non-adjacent destinations
 *
 * Passthroughs enable communication between zones that aren't directly connected.
 * They route RPC calls through intermediary zones, allowing hierarchical and
 * complex zone topologies.
 *
 * Example Topology:
 * Zone A ←transport_AB→ Zone B ←transport_BC→ Zone C
 *
 * Zone A can communicate with Zone C through Zone B's passthrough:
 * A → transport_AB → B (passthrough) → transport_BC → C
 *
 * Ownership Model:
 * - Passthroughs hold strong references to BOTH forward and reverse transports
 * - Passthroughs hold strong reference to intermediary service
 * - This keeps the entire routing path alive while passthrough exists
 * - Self-reference (self_ref_) prevents premature deletion during active calls
 *
 * See documents/architecture/06-transports-and-passthroughs.md for complete details.
 */

#pragma once

#include <rpc/internal/marshaller.h>
#include <rpc/internal/types.h>
#include <rpc/internal/coroutine_support.h>
#include <memory>
#include <atomic>

namespace rpc
{

    // Forward declarations
    class service;
    class transport;

    /**
     * @brief Routes RPC calls through an intermediary zone
     *
     * pass_through enables communication between non-adjacent zones by routing
     * calls through an intermediary zone. It implements i_marshaller to receive
     * calls and forwards them through the appropriate transport.
     *
     * Routing Logic:
     * - Calls arriving from forward_transport go to reverse_transport
     * - Calls arriving from reverse_transport go to forward_transport
     * - Determines direction based on destination_zone_id
     *
     * Reference Counting:
     * - Maintains its own shared_count_ and optimistic_count_
     * - Tracks references from remote zones
     * - Self-destructs when counts reach zero and no active calls
     *
     * Lifetime Management:
     * - Holds strong references to both transports (keeps routing path alive)
     * - Holds strong reference to service (keeps intermediary zone alive)
     * - Self-reference (self_ref_) prevents deletion during active calls
     * - combined_ tracks both active calls and shutdown state atomically
     *
     * Active Call Protection:
     * - combined_ low 63 bits count active in-flight calls
     * - begin_call() atomically checks-and-increments; rejects if shutting down
     * - end_call() decrements and triggers cleanup when last call finishes
     * - Prevents use-after-free during routing
     *
     * Disconnection Handling:
     * - combined_ bit 63 (SHUTDOWN_BIT) set when transport fails
     * - New calls atomically rejected when SHUTDOWN_BIT is set
     * - Cleanup occurs when SHUTDOWN_BIT is set and active call count reaches 0
     * - Self-reference cleared, allowing natural deletion
     *
     * Thread Safety:
     * - Reference counts use atomic operations
     * - Status changes use atomic operations
     * - Multiple threads can route calls concurrently
     *
     * See documents/architecture/06-transports-and-passthroughs.md for routing details.
     */
    class pass_through : public i_marshaller, public std::enable_shared_from_this<pass_through>
    {
    private:
        // Zone identifiers for routing decisions
        destination_zone forward_destination_; // Zone reached via forward_transport
        destination_zone reverse_destination_; // Zone reached via reverse_transport

        // Reference counts for passthrough lifetime
        std::atomic<uint64_t> shared_count_{0};
        std::atomic<uint64_t> optimistic_count_{0};

        // CRITICAL: Strong references to both transports keep routing path alive
        std::shared_ptr<transport> forward_transport_; // Transport to forward destination
        std::shared_ptr<transport> reverse_transport_; // Transport to reverse destination

        // CRITICAL: Strong reference to service keeps intermediary zone alive
        std::shared_ptr<service> service_;

        // Self-reference prevents deletion during active calls
        std::shared_ptr<pass_through> self_ref_;

        // Bit 63 = SHUTDOWN_BIT (set when disconnecting; rejects new calls atomically).
        // Bits 0..62 = count of active in-flight calls.
        // Combining both into one atomic eliminates the TOCTOU between "am I shutting
        // down?" and "can I safely start a new call?".
        static constexpr uint64_t SHUTDOWN_BIT = uint64_t(1) << 63;
        std::atomic<uint64_t> combined_{0};

        // Returns false (and does NOT increment) when SHUTDOWN_BIT is already set.
        bool begin_call();
        // Decrements the active call count. Calls do_cleanup() if SHUTDOWN_BIT is set
        // and we were the last active call.
        void end_call();
        // Atomically sets SHUTDOWN_BIT. Calls do_cleanup() immediately when there are
        // no active calls; otherwise end_call() will call it when the last call exits.
        // Safe to call multiple times — only the first caller performs cleanup.
        void trigger_self_destruction();
        // Performs the one-time teardown: removes passthrough from both transports and
        // releases all held strong references.
        void do_cleanup();

        rpc::zone zone_id_;

        pass_through(std::shared_ptr<transport> forward,
            std::shared_ptr<transport> reverse,
            std::shared_ptr<service> service,
            destination_zone forward_dest,
            destination_zone reverse_dest);

    public:
        static std::shared_ptr<pass_through> create(std::shared_ptr<transport> forward,
            std::shared_ptr<transport> reverse,
            std::shared_ptr<service> service,
            destination_zone forward_dest,
            destination_zone reverse_dest);
        ~pass_through();

        // i_marshaller implementations
        CORO_TASK(int)
        send(uint64_t protocol_version,
            encoding encoding,
            uint64_t tag,
            caller_zone caller_zone_id,
            remote_object remote_object_id,
            interface_ordinal interface_id,
            method method_id,
            const rpc::byte_span& in_data,
            std::vector<char>& out_buf_,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(void)
        post(uint64_t protocol_version,
            encoding encoding,
            uint64_t tag,
            caller_zone caller_zone_id,
            remote_object remote_object_id,
            interface_ordinal interface_id,
            method method_id,
            const rpc::byte_span& in_data,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        CORO_TASK(int)
        try_cast(uint64_t protocol_version,
            caller_zone caller_zone_id,
            remote_object remote_object_id,
            interface_ordinal interface_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(int)
        add_ref(uint64_t protocol_version,
            remote_object remote_object_id,
            caller_zone caller_zone_id,
            requesting_zone requesting_zone_id,
            add_ref_options build_out_param_channel,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(int)
        release(uint64_t protocol_version,
            remote_object remote_object_id,
            caller_zone caller_zone_id,
            release_options options,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(void)
        object_released(uint64_t protocol_version,
            remote_object remote_object_id,
            caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        CORO_TASK(void)
        transport_down(uint64_t protocol_version,
            destination_zone destination_zone_id,
            caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        CORO_TASK(int)
        get_new_zone_id(uint64_t protocol_version,
            zone& zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(void)
        local_transport_down(const std::shared_ptr<transport>& local_transport);

        // Status monitoring
        uint64_t get_shared_count() const { return shared_count_.load(std::memory_order_acquire); }
        uint64_t get_optimistic_count() const { return optimistic_count_.load(std::memory_order_acquire); }

        // Access to transports for testing
        std::shared_ptr<transport> get_forward_transport() const { return forward_transport_; }
        std::shared_ptr<transport> get_reverse_transport() const { return reverse_transport_; }

        destination_zone get_forward_destination() const
        {
            return forward_destination_;
        } // Zone reached via forward_transport
        destination_zone get_reverse_destination() const
        {
            return reverse_destination_;
        } // Zone reached via reverse_transport

        std::shared_ptr<transport> get_directional_transport(destination_zone dest);

        friend transport;
    };

} // namespace rpc
