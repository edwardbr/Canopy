/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <array>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <functional>

#include <coro/coro.hpp>
#include <rpc/rpc.h>
#include <spsc/spsc.h>
#include <transports/spsc/queue.h>

namespace rpc::spsc
{
    using message_blob = std::array<uint8_t, 10024>;
    using queue_type = ::spsc::queue<message_blob, 10024>;

    class spsc_transport : public rpc::transport
    {
    public:
        using connection_handler = std::function<CORO_TASK(int)(const rpc::interface_descriptor& input_descr,
            rpc::interface_descriptor& output_interface,
            std::shared_ptr<rpc::service> child_service_ptr,
            std::shared_ptr<spsc_transport>)>;

    private:
        struct result_listener
        {
            coro::event event;
            envelope_prefix prefix;
            envelope_payload payload;
            int error_code = rpc::error::OK();
            std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
            bool post_only = false;
        };

        std::unordered_map<uint64_t, result_listener*> pending_transmits_;
        std::mutex pending_transmits_mtx_;

        queue_type* send_spsc_queue_;
        queue_type* receive_spsc_queue_;

        std::atomic<uint64_t> sequence_number_ = 0;

        std::queue<std::vector<uint8_t>> send_queue_;
        std::mutex send_queue_mtx_;

        connection_handler connection_handler_;
        stdex::member_ptr<spsc_transport> keep_alive_;

        std::atomic<bool> peer_requested_disconnection_ = false;
        std::atomic<bool> pumps_started_ = false;

        struct activity_tracker
        {
            std::shared_ptr<spsc_transport> transport;
            std::shared_ptr<rpc::service> svc; // kept here to keep the service alive
            ~activity_tracker() { svc->get_scheduler()->spawn(transport->cleanup(transport, svc)); }
        };

        spsc_transport(std::string name,
            std::shared_ptr<rpc::service> service,
            rpc::zone adjacent_zone_id,
            queue_type* send_spsc_queue,
            queue_type* receive_spsc_queue,
            connection_handler handler);

        // Producer/consumer coroutines
        CORO_TASK(void) receive_consumer_loop(std::shared_ptr<activity_tracker> tracker);
        CORO_TASK(void) send_producer_loop(std::shared_ptr<activity_tracker> tracker);

        enum send_queue_status
        {
            SEND_QUEUE_EMPTY,
            SEND_QUEUE_NOT_EMPTY,
            SPSC_QUEUE_FULL
        };
        send_queue_status push_message(std::span<uint8_t>& send_data);

        // Stub handlers (called when receiving messages)
        CORO_TASK(void)
        stub_handle_send(std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        stub_handle_try_cast(std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        stub_handle_add_ref(std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        stub_handle_release(std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        stub_handle_post(std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        stub_handle_object_released(
            std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        stub_handle_transport_down(
            std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void)
        create_stub(std::shared_ptr<activity_tracker> tracker, envelope_prefix prefix, envelope_payload payload);

        template<class SendPayload>
        void send_payload(
            std::uint64_t protocol_version, message_direction direction, SendPayload&& sendPayload, uint64_t sequence_number)
        {
            assert(direction);

            envelope_payload payload_envelope = {.payload_fingerprint = rpc::id<SendPayload>::get(protocol_version),
                .payload = rpc::to_yas_binary(sendPayload)};
            auto payload = rpc::to_yas_binary(payload_envelope);

            auto prefix = envelope_prefix{.version = protocol_version,
                .direction = direction,
                .sequence_number = sequence_number,
                .payload_size = payload.size()};

            RPC_DEBUG("send_payload {}\nprefix = {}\npayload = {}",
                get_service()->get_zone_id().get_val(),
                rpc::to_yas_json<std::string>(prefix),
                rpc::to_yas_json<std::string>(payload_envelope));

            std::scoped_lock g(send_queue_mtx_);
            send_queue_.push(rpc::to_yas_binary(prefix));
            send_queue_.push(payload);
        }

        // Send and wait for reply (used internally)
        template<class SendPayload, class ReceivePayload>
        CORO_TASK(int)
        call_peer(std::uint64_t protocol_version, SendPayload&& sendPayload, ReceivePayload& receivePayload)
        {
            if (get_status() != rpc::transport_status::CONNECTED && get_status() != rpc::transport_status::CONNECTING)
            {
                RPC_ERROR("call_peer: transport is not connected");
                CO_RETURN rpc::error::CALL_CANCELLED();
            }

            // If peer has initiated shutdown, we're disconnected
            auto sequence_number = ++sequence_number_;

            // Register the receive listener before we do the send
            result_listener res_payload;

            {
                RPC_DEBUG("call_peer started zone: {} sequence_number: {} id: {}",
                    get_service()->get_zone_id().get_val(),
                    sequence_number,
                    rpc::id<SendPayload>::get(rpc::get_version()));
                std::scoped_lock lock(pending_transmits_mtx_);
                auto [it, success] = pending_transmits_.try_emplace(sequence_number, &res_payload);
                if (!success)
                {
                    RPC_ERROR("call_peer: failed to insert sequence number {}", sequence_number);
                    CO_RETURN rpc::error::TRANSPORT_ERROR();
                }
            }

            send_payload(protocol_version, message_direction::send, std::move(sendPayload), sequence_number);

            CO_AWAIT res_payload.event; // now wait for the reply

            RPC_DEBUG("call_peer succeeded zone: {} sequence_number: {} id: {}",
                get_service()->get_zone_id().get_val(),
                sequence_number,
                rpc::id<SendPayload>::get(rpc::get_version()));

            // Check if the operation was cancelled during shutdown
            if (res_payload.error_code != rpc::error::OK())
            {
                RPC_ERROR("call_peer returning cancelled error for zone: {} sequence_number: {}",
                    get_service()->get_zone_id().get_val(),
                    sequence_number);
                CO_RETURN res_payload.error_code;
            }

            auto str_err = rpc::from_yas_binary(rpc::span(res_payload.payload.payload), receivePayload);
            if (!str_err.empty())
            {
                RPC_ERROR("failed call_peer send_payload from_yas_binary");
                CO_RETURN rpc::error::PROXY_DESERIALISATION_ERROR();
            }

            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(void) cleanup(std::shared_ptr<spsc_transport> transport, std::shared_ptr<rpc::service> svc);

    public:
        static std::shared_ptr<spsc_transport> create(std::string name,
            std::shared_ptr<rpc::service> service,
            rpc::zone adjacent_zone_id,
            queue_type* send_spsc_queue,
            queue_type* receive_spsc_queue,
            connection_handler handler);

        virtual ~spsc_transport() { };

        void pump_send_and_receive();

        // Internal send payload helper
        // rpc::transport override - connect handshake
        CORO_TASK(int)
        inner_connect(rpc::interface_descriptor input_descr, rpc::interface_descriptor& output_descr) override;

        CORO_TASK(int) inner_accept() override;

        // outbound i_marshaller implementations (from rpc::transport)
        CORO_TASK(int)
        outbound_send(uint64_t protocol_version,
            rpc::encoding encoding,
            uint64_t tag,
            rpc::caller_zone caller_zone_id,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::interface_ordinal interface_id,
            rpc::method method_id,
            const rpc::span& in_data,
            std::vector<char>& out_buf_,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(void)
        outbound_post(uint64_t protocol_version,
            rpc::encoding encoding,
            uint64_t tag,
            rpc::caller_zone caller_zone_id,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::interface_ordinal interface_id,
            rpc::method method_id,
            const rpc::span& in_data,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        CORO_TASK(int)
        outbound_try_cast(uint64_t protocol_version,
            rpc::caller_zone caller_zone_id,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::interface_ordinal interface_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(int)
        outbound_add_ref(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::caller_zone caller_zone_id,
            rpc::known_direction_zone known_direction_zone_id,
            rpc::add_ref_options build_out_param_channel,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(int)
        outbound_release(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::caller_zone caller_zone_id,
            rpc::release_options options,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        // New methods from i_marshaller interface
        CORO_TASK(void)
        outbound_object_released(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::object object_id,
            rpc::caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        CORO_TASK(void)
        outbound_transport_down(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;
    };
}
