/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <queue>
#include <unordered_map>

#include <coro/coro.hpp>
#include <rpc/rpc.h>
#include <stream_transport/stream_transport.h>
#include <streaming/stream.h>

namespace rpc::stream_transport
{
    class transport : public rpc::transport
    {
    public:
        enum class message_hook_result
        {
            unhandled,
            handled,
            rejected
        };

        using connection_handler = rpc::connection_handler;

        // Server-side: creates a transport that waits for the client's init message.
        // Internally wraps factory with make_new_zone_connection_handler so connection protocol details
        // stay hidden from user code.
        template<class Remote, class Local>
        static std::shared_ptr<transport> make_server(std::string name,
            std::shared_ptr<rpc::service> service,
            std::shared_ptr<streaming::stream> stream,
            std::function<CORO_TASK(int)(
                const rpc::shared_ptr<Remote>&, rpc::shared_ptr<Local>&, const std::shared_ptr<rpc::service>&)> factory)
        {
            auto handler = rpc::make_new_zone_connection_handler<Remote, Local>(name.c_str(), std::move(factory));
            return make_server(std::move(name), std::move(service), std::move(stream), std::move(handler));
        }

        // Client-side: creates a transport that sends the init message to the server.
        static std::shared_ptr<transport> make_client(
            std::string name, std::shared_ptr<rpc::service> service, std::shared_ptr<streaming::stream> stream)
        {
            return make_server(std::move(name), std::move(service), std::move(stream), nullptr);
        }

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

        struct activity_tracker;

        using custom_message_handler = std::function<CORO_TASK(message_hook_result)(
            std::shared_ptr<activity_tracker> tracker, envelope_prefix& prefix, envelope_payload& payload)>;

        using custom_outgoing_handler = std::function<bool(
            uint64_t protocol_version, message_direction direction, uint64_t sequence_number, uint64_t type_fingerprint, const void* typed_payload)>;

        std::unordered_map<uint64_t, result_listener*> pending_transmits_;
        std::mutex pending_transmits_mtx_;

        std::shared_ptr<streaming::stream> stream_;

        std::atomic<uint64_t> sequence_number_ = 0;

        std::queue<std::vector<uint8_t>> send_queue_;
        std::mutex send_queue_mtx_;

        connection_handler connection_handler_;
        std::vector<custom_message_handler> custom_message_handlers_;
        std::vector<custom_outgoing_handler> custom_outgoing_handlers_;
        stdex::member_ptr<transport> keep_alive_;

        std::shared_ptr<rpc::object_stub> stub_;

        std::atomic<bool> peer_requested_disconnection_ = false;
        std::atomic<bool> pumps_started_ = false;

        // Two-phase close protocol state
        std::atomic<bool> send_cleanup_done_ = false;
        std::chrono::steady_clock::time_point disconnecting_since_{};
        static constexpr uint32_t shutdown_timeout_ms_ = 5000;

        struct activity_tracker
        {
            std::shared_ptr<transport> transport;
            std::shared_ptr<rpc::service> svc; // kept here to keep the service alive
            ~activity_tracker() { svc->spawn(transport->cleanup(transport, svc)); }
        };

        transport(std::string name,
            std::shared_ptr<rpc::service> service,
            std::shared_ptr<streaming::stream> stream,
            connection_handler handler);

        // Producer/consumer coroutines
        CORO_TASK(void) receive_consumer_loop(std::shared_ptr<activity_tracker> tracker);
        CORO_TASK(void) send_producer_loop(std::shared_ptr<activity_tracker> tracker);
        CORO_TASK(void) flush_send_queue();

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
        CORO_TASK(message_hook_result)
        run_custom_message_handlers(
            std::shared_ptr<activity_tracker> tracker, envelope_prefix& prefix, envelope_payload& payload);
        CORO_TASK(bool)
        dispatch_builtin_message(
            std::shared_ptr<activity_tracker> tracker, envelope_prefix& prefix, envelope_payload& payload);

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

            RPC_TRACE("send_payload {}\nprefix = {}\npayload = {}",
                get_service()->get_zone_id().get_subnet(),
                rpc::to_yas_json<std::string>(prefix),
                rpc::to_yas_json<std::string>(payload_envelope));

            std::scoped_lock g(send_queue_mtx_);
            send_queue_.push(rpc::to_yas_binary(prefix));
            send_queue_.push(payload);
        }

        template<class SendPayload, class ReceivePayload>
        CORO_TASK(int)
        call_peer(std::uint64_t protocol_version, SendPayload&& sendPayload, ReceivePayload& receivePayload)
        {
            if (get_status() != rpc::transport_status::CONNECTED)
            {
                RPC_ERROR("call_peer: transport is not connected");
                CO_RETURN rpc::error::CALL_CANCELLED();
            }

            auto sequence_number = ++sequence_number_;

            result_listener res_payload;

            {
                RPC_TRACE("call_peer started zone: {} sequence_number: {} id: {}",
                    get_service()->get_zone_id().get_subnet(),
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

            CO_AWAIT res_payload.event; // wait for the reply

            RPC_TRACE("call_peer succeeded zone: {} sequence_number: {} id: {}",
                get_service()->get_zone_id().get_subnet(),
                sequence_number,
                rpc::id<SendPayload>::get(rpc::get_version()));

            if (res_payload.error_code == rpc::error::OBJECT_GONE())
            {
                CO_RETURN res_payload.error_code;
            }
            if (rpc::error::is_critical(res_payload.error_code))
            {
                RPC_ERROR("call_peer returning cancelled error for zone: {} sequence_number: {}",
                    get_service()->get_zone_id().get_subnet(),
                    sequence_number);
                CO_RETURN res_payload.error_code;
            }

            auto str_err = rpc::from_yas_binary(rpc::byte_span(res_payload.payload.payload), receivePayload);
            if (!str_err.empty())
            {
                RPC_ERROR("failed call_peer send_payload from_yas_binary");
                CO_RETURN rpc::error::PROXY_DESERIALISATION_ERROR();
            }

            CO_RETURN rpc::error::OK();
        }

        CORO_TASK(void) cleanup(std::shared_ptr<transport> transport, std::shared_ptr<rpc::service> svc);

        template<class T>
        bool run_outgoing_handlers(
            uint64_t protocol_version, message_direction direction, uint64_t sequence_number, const T& payload)
        {
            auto fingerprint = rpc::id<T>::get(protocol_version);
            for (auto& handler : custom_outgoing_handlers_)
            {
                if (handler(protocol_version, direction, sequence_number, fingerprint, &payload))
                    return true;
            }
            return false;
        }

    protected:
        void on_destination_count_zero() override { set_status(rpc::transport_status::DISCONNECTING); }

        void set_status(rpc::transport_status new_status) override
        {
            if (new_status == rpc::transport_status::DISCONNECTING)
            {
                disconnecting_since_ = std::chrono::steady_clock::now();
            }
            rpc::transport::set_status(new_status);
        }

        virtual void send_payload_post_send(
            uint64_t protocol_version, message_direction direction, post_send&& payload, uint64_t sequence_number);
        virtual void send_payload_release_send(
            uint64_t protocol_version, message_direction direction, release_send&& payload, uint64_t sequence_number);
        virtual void send_payload_object_released_send(
            uint64_t protocol_version, message_direction direction, object_released_send&& payload, uint64_t sequence_number);
        virtual void send_payload_transport_down_send(
            uint64_t protocol_version, message_direction direction, transport_down_send&& payload, uint64_t sequence_number);
        virtual void send_payload_close_connection_ack(
            uint64_t protocol_version, message_direction direction, close_connection_ack&& payload, uint64_t sequence_number);
        virtual void send_payload_close_connection_send(uint64_t protocol_version,
            message_direction direction,
            close_connection_send&& payload,
            uint64_t sequence_number);
        virtual void send_payload_call_receive(
            uint64_t protocol_version, message_direction direction, call_receive&& payload, uint64_t sequence_number);
        virtual void send_payload_try_cast_receive(
            uint64_t protocol_version, message_direction direction, try_cast_receive&& payload, uint64_t sequence_number);
        virtual void send_payload_addref_receive(
            uint64_t protocol_version, message_direction direction, addref_receive&& payload, uint64_t sequence_number);
        virtual void send_payload_init_client_initial_channel_response(uint64_t protocol_version,
            message_direction direction,
            init_client_initial_channel_response&& payload,
            uint64_t sequence_number);
        virtual void send_payload_init_client_channel_response(uint64_t protocol_version,
            message_direction direction,
            init_client_channel_response&& payload,
            uint64_t sequence_number);

    public:
        static std::shared_ptr<transport> make_server(std::string name,
            std::shared_ptr<rpc::service> service,
            std::shared_ptr<streaming::stream> stream,
            connection_handler handler);

        ~transport() override { }

        void add_custom_message_handler(custom_message_handler handler)
        {
            custom_message_handlers_.push_back(std::move(handler));
        }

        template<class T, class Handler> void add_typed_message_handler(Handler&& handler)
        {
            custom_message_handlers_.push_back(
                [fn = std::forward<Handler>(handler)](std::shared_ptr<activity_tracker> tracker,
                    envelope_prefix& prefix,
                    envelope_payload& payload) -> CORO_TASK(message_hook_result)
                {
                    if (payload.payload_fingerprint != rpc::id<T>::get(prefix.version))
                        CO_RETURN message_hook_result::unhandled;

                    T request;
                    auto err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
                    if (!err.empty())
                    {
                        RPC_ERROR("failed custom message deserialisation");
                        CO_RETURN message_hook_result::rejected;
                    }

                    CO_RETURN CO_AWAIT fn(tracker, prefix, payload, request);
                });
        }

        template<class T, class Handler> void add_typed_outgoing_message_handler(Handler&& handler)
        {
            custom_outgoing_handlers_.push_back(
                [fn = std::forward<Handler>(handler)](uint64_t protocol_version,
                    message_direction direction,
                    uint64_t sequence_number,
                    uint64_t type_fingerprint,
                    const void* typed_payload) -> bool
                {
                    if (type_fingerprint != rpc::id<T>::get(protocol_version))
                        return false;
                    return fn(protocol_version, direction, sequence_number, *static_cast<const T*>(typed_payload));
                });
        }

        template<class T> void reject_message_type()
        {
            custom_message_handlers_.push_back(
                [](std::shared_ptr<activity_tracker>,
                    envelope_prefix& prefix,
                    envelope_payload& payload) -> CORO_TASK(message_hook_result)
                {
                    if (payload.payload_fingerprint == rpc::id<T>::get(prefix.version))
                        CO_RETURN message_hook_result::rejected;

                    CO_RETURN message_hook_result::unhandled;
                });
        }

        CORO_TASK(void) pump_send_and_receive();

        CORO_TASK(int)
        run_custom_connect(const rpc::remote_object& inbound_remote_object,
            rpc::interface_ordinal inbound_interface_id,
            rpc::interface_ordinal outbound_interface_id,
            rpc::interface_descriptor& output_interface)
        {
            RPC_DEBUG("custom connect handler zone: {}", get_zone_id().get_subnet());

            rpc::connection_settings input_descr;
            input_descr.inbound_interface_id = inbound_interface_id;
            input_descr.outbound_interface_id = outbound_interface_id;
            input_descr.remote_object_id = inbound_remote_object;

            set_adjacent_zone_id(inbound_remote_object.as_zone());

            int ret
                = CO_AWAIT connection_handler_(input_descr, output_interface, get_service(), keep_alive_.get_nullable());
            connection_handler_ = nullptr;
            if (ret != rpc::error::OK())
            {
                RPC_ERROR("failed custom connect to zone {}", ret);
                set_status(rpc::transport_status::DISCONNECTING);
            }

            CO_RETURN ret;
        }

        template<class ResponsePayload>
        void send_custom_connect_response(uint64_t protocol_version,
            uint64_t sequence_number,
            rpc::zone caller_zone_id,
            rpc::remote_object outbound_remote_object)
        {
            send_payload(protocol_version,
                message_direction::receive,
                ResponsePayload{
                    .caller_zone_id = caller_zone_id,
                    .outbound_remote_object = outbound_remote_object,
                },
                sequence_number);
        }

        CORO_TASK(int)
        inner_connect(const std::shared_ptr<rpc::object_stub>& stub,
            connection_settings& input_descr,
            rpc::interface_descriptor& output_descr) override;

        CORO_TASK(int) inner_accept() override;

        CORO_TASK(int)
        outbound_send(uint64_t protocol_version,
            rpc::encoding encoding,
            uint64_t tag,
            rpc::caller_zone caller_zone_id,
            rpc::remote_object remote_object_id,
            rpc::interface_ordinal interface_id,
            rpc::method method_id,
            const rpc::byte_span& in_data,
            std::vector<char>& out_buf_,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(void)
        outbound_post(uint64_t protocol_version,
            rpc::encoding encoding,
            uint64_t tag,
            rpc::caller_zone caller_zone_id,
            rpc::remote_object remote_object_id,
            rpc::interface_ordinal interface_id,
            rpc::method method_id,
            const rpc::byte_span& in_data,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        CORO_TASK(int)
        outbound_try_cast(uint64_t protocol_version,
            rpc::caller_zone caller_zone_id,
            rpc::remote_object remote_object_id,
            rpc::interface_ordinal interface_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(int)
        outbound_add_ref(uint64_t protocol_version,
            rpc::remote_object remote_object_id,
            rpc::caller_zone caller_zone_id,
            rpc::requesting_zone requesting_zone_id,
            rpc::add_ref_options build_out_param_channel,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(int)
        outbound_release(uint64_t protocol_version,
            rpc::remote_object remote_object_id,
            rpc::caller_zone caller_zone_id,
            rpc::release_options options,
            const std::vector<rpc::back_channel_entry>& in_back_channel,
            std::vector<rpc::back_channel_entry>& out_back_channel) override;

        CORO_TASK(void)
        outbound_object_released(uint64_t protocol_version,
            rpc::remote_object remote_object_id,
            rpc::caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;

        CORO_TASK(void)
        outbound_transport_down(uint64_t protocol_version,
            rpc::destination_zone destination_zone_id,
            rpc::caller_zone caller_zone_id,
            const std::vector<rpc::back_channel_entry>& in_back_channel) override;
    };

    // Returns a transport_factory that creates a streaming server transport wrapping
    // the given stream. Pass the result to service::make_acceptor().
    inline rpc::transport_factory transport_factory(std::shared_ptr<streaming::stream> stream)
    {
        return [stream = std::move(stream)](std::string name,
                   std::shared_ptr<rpc::service> svc,
                   rpc::connection_handler handler) -> CORO_TASK(std::shared_ptr<rpc::transport>)
        { CO_RETURN transport::make_server(std::move(name), std::move(svc), stream, std::move(handler)); };
    }

    // Produces a connection_callback for use with streaming::listener.
    // Wraps a typed zone factory so the listener does not need to know about
    // connection_handler or make_new_zone_connection_handler.
    template<class Remote, class Local>
    std::function<CORO_TASK(void)(const std::string&, std::shared_ptr<rpc::service>, std::shared_ptr<streaming::stream>)>
    make_connection_callback(std::function<CORO_TASK(int)(
            const rpc::shared_ptr<Remote>&, rpc::shared_ptr<Local>&, const std::shared_ptr<rpc::service>&)> factory)
    {
        return [fn = std::move(factory)](const std::string& name,
                   std::shared_ptr<rpc::service> svc,
                   std::shared_ptr<streaming::stream> stm) -> CORO_TASK(void)
        {
            transport::make_server<Remote, Local>(name, std::move(svc), std::move(stm), fn);
            CO_RETURN;
        };
    }
}
