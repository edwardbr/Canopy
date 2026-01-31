/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <functional>
#include <mutex>
#include <queue>
#include <unordered_map>

#include <coro/coro.hpp>
#include <coro/net/tcp/client.hpp>
#include <rpc/rpc.h>
#include <tcp/tcp.h>

namespace rpc::tcp
{
    class tcp_transport : public rpc::transport
    {
    public:
        using connection_handler = std::function<CORO_TASK(int)(const rpc::interface_descriptor& input_descr,
            rpc::interface_descriptor& output_interface,
            std::shared_ptr<rpc::service> child_service_ptr,
            std::shared_ptr<tcp_transport>)>;

    private:
        struct result_listener
        {
            rpc::event event;
            envelope_prefix prefix;
            envelope_payload payload;
            int error_code = rpc::error::OK();
            std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
        };

        coro::net::tcp::client client_;
        std::chrono::milliseconds timeout_;

        std::atomic<uint64_t> sequence_number_ = 0;

        // this is the queue of blobs awaiting to be sent over the wire
        coro::mutex send_queue_mtx_;
        std::queue<std::vector<uint8_t>> send_queue_;
        std::atomic<size_t> send_queue_count_{0};

        // this is a register of blobs that have been sent but have not received a reply yet
        std::mutex pending_transmits_mtx_;
        std::unordered_map<uint64_t, result_listener*> pending_transmits_;
        std::atomic<size_t> pending_transmits_count_{0};

        connection_handler connection_handler_;
        stdex::member_ptr<tcp_transport> keep_alive_;

        // Reference counting for shutdown sequence completion
        // std::atomic<int> shutdown_sequence_completed_{0};

        tcp_transport(std::string name,
            std::shared_ptr<rpc::service> service,
            rpc::zone adjacent_zone_id,
            std::chrono::milliseconds timeout,
            coro::net::tcp::client client,
            connection_handler handler);

        // Producer/consumer coroutines
        CORO_TASK(void)
        pump_messages(std::function<void(envelope_prefix, envelope_payload)> incoming_message_handler);

        // Stub handlers (called when receiving messages)
        CORO_TASK(void) stub_handle_send(envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void) stub_handle_try_cast(envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void) stub_handle_add_ref(envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void) stub_handle_release(envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void) stub_handle_post(envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void) stub_handle_object_released(envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void) stub_handle_transport_down(envelope_prefix prefix, envelope_payload payload);
        CORO_TASK(void) create_stub(envelope_prefix prefix, envelope_payload payload);

        template<class SendPayload>
        CORO_TASK(int)
        send_payload(
            std::uint64_t protocol_version, message_direction direction, SendPayload&& sendPayload, uint64_t sequence_number)
        {
            assert(direction);
            auto scoped_lock = CO_AWAIT send_queue_mtx_.lock();

            envelope_payload payload_envelope = {.payload_fingerprint = rpc::id<SendPayload>::get(protocol_version),
                .payload = rpc::to_compressed_yas_binary(sendPayload)};
            auto payload = rpc::to_yas_binary(payload_envelope);

            auto prefix = envelope_prefix{.version = protocol_version,
                .direction = direction,
                .sequence_number = sequence_number,
                .payload_size = payload.size()};

            RPC_DEBUG("send_payload {}\nprefix = {}\npayload = {}",
                get_service()->get_zone_id().get_val(),
                rpc::to_yas_json<std::string>(prefix),
                rpc::to_yas_json<std::string>(payload_envelope));

            send_queue_.push(rpc::to_yas_binary(prefix));
            send_queue_.push(payload);
            send_queue_count_.fetch_add(2, std::memory_order_release);

            CO_RETURN rpc::error::OK();
        }

        template<class SendPayload, class ReceivePayload>
        CORO_TASK(int)
        call_peer(std::uint64_t protocol_version, SendPayload&& sendPayload, ReceivePayload& receivePayload)
        {
            // If peer has initiated shutdown, we're disconnected
            if (get_status() != rpc::transport_status::CONNECTED && get_status() != rpc::transport_status::CONNECTING)
            {
                RPC_DEBUG("call_peer: shutting_down_=true, returning CALL_CANCELLED for zone {}",
                    get_service()->get_zone_id().get_val());
                CO_RETURN rpc::error::CALL_CANCELLED();
            }

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
                pending_transmits_count_.fetch_add(1, std::memory_order_release);
            }

            auto err = CO_AWAIT send_payload(
                protocol_version, message_direction::send, std::move(sendPayload), sequence_number);
            if (err != rpc::error::OK())
            {
                RPC_ERROR("failed call_peer send_payload send");
                std::scoped_lock lock(pending_transmits_mtx_);
                RPC_ERROR("call_peer failed zone: {} sequence_number: {} id: {}",
                    get_service()->get_zone_id().get_val(),
                    sequence_number,
                    rpc::id<SendPayload>::get(rpc::get_version()));
                pending_transmits_.erase(sequence_number);
                pending_transmits_count_.fetch_sub(1, std::memory_order_release);
                CO_RETURN err;
            }

            CO_AWAIT res_payload.event.wait(); // now wait for the reply

            RPC_DEBUG("call_peer succeeded zone: {} sequence_number: {} id: {}",
                get_service()->get_zone_id().get_val(),
                sequence_number,
                rpc::id<SendPayload>::get(rpc::get_version()));

            // Check if the operation was cancelled during shutdown
            if (res_payload.error_code != rpc::error::OK())
            {
                RPC_DEBUG("call_peer returning cancelled error for zone: {} sequence_number: {}",
                    get_service()->get_zone_id().get_val(),
                    sequence_number);
                CO_RETURN res_payload.error_code;
            }

            assert(res_payload.payload.payload_fingerprint == rpc::id<ReceivePayload>::get(res_payload.prefix.version));

            auto str_err = rpc::from_yas_compressed_binary(rpc::span(res_payload.payload.payload), receivePayload);
            if (!str_err.empty())
            {
                RPC_ERROR("failed call_peer send_payload from_yas_compressed_binary");
                CO_RETURN rpc::error::TRANSPORT_ERROR();
            }

            CO_RETURN rpc::error::OK();
        }

    public:
        static std::shared_ptr<tcp_transport> create(std::string name,
            std::shared_ptr<rpc::service> service,
            rpc::zone adjacent_zone_id,
            std::chrono::milliseconds timeout,
            coro::net::tcp::client client,
            connection_handler handler);

        virtual ~tcp_transport() { };

        // Override set_status to trigger service shutdown event when disconnecting
        void set_status(transport_status new_status) override;

        CORO_TASK(void) pump_send_and_receive();

        // Internal send payload helper
        // rpc::transport override - connect handshake
        CORO_TASK(int)
        inner_connect(rpc::interface_descriptor input_descr, rpc::interface_descriptor& output_descr) override;

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
