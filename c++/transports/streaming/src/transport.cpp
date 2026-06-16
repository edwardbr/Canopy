/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <atomic>
#include <cerrno>
#include <transports/streaming/transport.h>

namespace rpc::stream_transport
{
    namespace
    {
        constexpr int epipe_native_code =
#ifdef EPIPE
            EPIPE;
#else
            32;
#endif

        constexpr int econnreset_native_code =
#ifdef ECONNRESET
            ECONNRESET;
#else
            104;
#endif

        auto is_peer_disconnect_send_failure(const rpc::io_status& send_status) -> bool
        {
            if (send_status.is_closed() || send_status.type == rpc::io_status::kind::connection_reset)
                return true;

            return send_status.type == rpc::io_status::kind::native
                   && (send_status.native_code == epipe_native_code || send_status.native_code == econnreset_native_code);
        }

    } // namespace

    transport::transport(
        std::string name,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<streaming::stream> stream,
        connection_handler handler,
        stream_transport_options options)
        : rpc::transport(
              name,
              service)
        , stream_(std::move(stream))
        , connection_handler_(std::move(handler))
        , call_timeout_(options.call_timeout)
        , call_timeout_sweep_(options.call_timeout_sweep)
        , shutdown_timeout_(options.shutdown_timeout)
    {
    }

    void transport::initialise_after_construction()
    {
        keep_alive_ = std::static_pointer_cast<rpc::stream_transport::transport>(shared_from_this());
        set_status(rpc::transport_status::CONNECTED);

        if (connection_handler_)
        {
            // Server-side pump kick-off. If the executor refuses the spawn
            // (shut down, missing in blocking mode) the transport can't
            // serve incoming messages — mark it DISCONNECTING so callers
            // surface the failure instead of silently accepting connections
            // that will never make progress.
            if (!get_service()->SPAWN(pump_send_and_receive()))
            {
                RPC_ERROR(
                    "initialise_after_construction: failed to spawn pump for zone {} — disconnecting",
                    get_zone_id().get_subnet());
                set_status(rpc::transport_status::DISCONNECTING);
            }
        }
    }

    void transport::set_status(rpc::transport_status new_status)
    {
        auto transition = try_advance_status(new_status);
        if (transition.changed)
        {
            RPC_DEBUG(
                "stream transport status zone={} adjacent={} old={} new={}",
                get_zone_id().get_subnet(),
                get_adjacent_zone_id().get_subnet(),
                static_cast<int>(transition.old_status),
                static_cast<int>(new_status));

            if (new_status == rpc::transport_status::DISCONNECTING && shutdown_timeout_.count() > 0)
                disconnecting_since_ = std::chrono::steady_clock::now();
            if (transition.old_status < rpc::transport_status::DISCONNECTING
                && new_status >= rpc::transport_status::DISCONNECTING)
                on_disconnecting();
        }
        else if (transition.rejected_regression)
        {
            RPC_DEBUG(
                "stream transport ignored stale status zone={} adjacent={} old={} requested={}",
                get_zone_id().get_subnet(),
                get_adjacent_zone_id().get_subnet(),
                static_cast<int>(transition.old_status),
                static_cast<int>(new_status));
        }

        if (new_status != rpc::transport_status::CONNECTED)
        {
            send_queue_ready_.set();
        }
    }

    std::shared_ptr<transport> create(
        std::string name,
        std::shared_ptr<rpc::service> service,
        std::shared_ptr<streaming::stream> stream,
        transport::connection_handler handler,
        stream_transport_options options)
    {
        auto transport = std::shared_ptr<rpc::stream_transport::transport>(
            new rpc::stream_transport::transport(name, service, std::move(stream), std::move(handler), options));

        transport->initialise_after_construction();

        return transport;
    }

    CORO_TASK(rpc::connect_result)
    transport::inner_connect(
        std::shared_ptr<rpc::object_stub> stub,
        connection_settings input_descr)
    {
        RPC_DEBUG("stream_transport::transport::inner_connect zone={}", get_zone_id().get_subnet());
        stub_ = stub;

        auto service = get_service();
        RPC_DEBUG("inner_connect: spawning pump for zone {}", get_zone_id().get_subnet());
        if (!service->SPAWN(pump_send_and_receive()))
        {
            RPC_ERROR("inner_connect: failed to spawn pump for zone {}", get_zone_id().get_subnet());
            stub_.reset();
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN rpc::connect_result{rpc::error::TRANSPORT_ERROR(), {}};
        }
        RPC_DEBUG("inner_connect: pump spawned, calling call_peer for zone {}", get_zone_id().get_subnet());

        if (!connection_handler_)
        {
            // Client side: send init message to server
            auto inbound_remote_object_r = get_zone_id().with_object(input_descr.get_object_id());
            if (!inbound_remote_object_r)
            {
                CO_RETURN rpc::connect_result{rpc::error::INVALID_DATA(), {}};
            }
            auto init_result = CO_AWAIT call_peer<init_client_channel_send, init_client_channel_response>(
                rpc::get_version(),
                init_client_channel_send{FLD(inbound_remote_object) std::move(*inbound_remote_object_r),
                    FLD(inbound_interface_id) input_descr.inbound_interface_id,
                    FLD(destination_zone_id) get_adjacent_zone_id(),
                    FLD(outbound_interface_id) input_descr.outbound_interface_id,
                    FLD(encoding_type) input_descr.encoding_type,
                    FLD(adjacent_zone_id) get_zone_id()});
            int ret = init_result.error_code;
            if (ret != rpc::error::OK())
            {
                stub_.reset();
                RPC_ERROR("stream_transport::transport::inner_connect call_peer failed {}", rpc::error::to_string(ret));
                CO_RETURN rpc::connect_result{ret, {}};
            }

            auto& init_receive = init_result.payload;

            if (init_receive.err_code != rpc::error::OK())
            {
                stub_.reset();
                RPC_ERROR("init_client_channel_send failed {}", rpc::error::to_string(init_receive.err_code));
                CO_RETURN rpc::connect_result{init_receive.err_code, {}};
            }

            CO_RETURN rpc::connect_result{rpc::error::OK(), init_receive.outbound_remote_object};
        }

        CO_RETURN rpc::connect_result{rpc::error::OK(), {}};
    }

    CORO_TASK(int) transport::inner_accept()
    {
        // Pump is already running — started by create() when connection_handler was provided.
        CO_RETURN rpc::error::OK();
    }

    CORO_TASK(send_result)
    transport::outbound_send(send_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_send zone={}", get_zone_id().get_subnet());

        auto response_result = CO_AWAIT call_peer<call_send, call_receive>(
            params.protocol_version,
            call_send{FLD(encoding) params.encoding_type,
                FLD(tag) params.tag,
                FLD(request_id) params.request_id,
                FLD(caller_zone_id) params.caller_zone_id,
                FLD(destination_zone_id) params.remote_object_id,
                FLD(interface_id) params.interface_id,
                FLD(method_id) params.method_id,
                FLD(payload) std::move(params.in_data),
                FLD(back_channel) std::move(params.in_back_channel)});
        int ret = response_result.error_code;

        if (rpc::error::is_error(ret))
        {
            RPC_DEBUG("failed stream_transport::transport::outbound_send call_send");
            CO_RETURN send_result{ret, {}, {}};
        }

        auto& response = response_result.payload;
        RPC_DEBUG("stream_transport::transport::outbound_send complete zone={}", get_zone_id().get_subnet());
        CO_RETURN send_result{response.err_code, std::move(response.payload), std::move(response.back_channel)};
    }

    CORO_TASK(void)
    transport::outbound_post(post_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_post zone={}", get_zone_id().get_subnet());

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            RPC_ERROR("stream_transport::transport::outbound_post: transport not connected");
            CO_RETURN;
        }

        send_payload_post_send(
            params.protocol_version,
            message_direction::one_way,
            post_send{FLD(encoding) params.encoding_type,
                FLD(tag) params.tag,
                FLD(caller_zone_id) params.caller_zone_id,
                FLD(destination_zone_id) params.remote_object_id,
                FLD(interface_id) params.interface_id,
                FLD(method_id) params.method_id,
                FLD(payload) std::move(params.in_data),
                FLD(back_channel) std::move(params.in_back_channel)},
            0);

        CO_RETURN;
    }

    CORO_TASK(standard_result)
    transport::outbound_try_cast(try_cast_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_try_cast zone={}", get_zone_id().get_subnet());

        auto response_result = CO_AWAIT call_peer<try_cast_send, try_cast_receive>(
            params.protocol_version,
            try_cast_send{FLD(caller_zone_id) params.caller_zone_id,
                FLD(destination_zone_id) params.remote_object_id,
                FLD(interface_id) params.interface_id,
                FLD(back_channel) std::move(params.in_back_channel),
                FLD(payload) std::move(params.payload)});
        int ret = response_result.error_code;
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed try_cast call_peer");
            CO_RETURN standard_result{ret, {}};
        }

        auto& response_data = response_result.payload;
        CO_RETURN standard_result{response_data.err_code, std::move(response_data.back_channel)};
    }

    CORO_TASK(get_schema_result)
    transport::outbound_get_schema(get_schema_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_get_schema zone={}", get_zone_id().get_subnet());

        auto response_result = CO_AWAIT call_peer<get_schema_send, get_schema_receive>(
            params.protocol_version,
            get_schema_send{FLD(caller_zone_id) params.caller_zone_id,
                FLD(destination_zone_id) params.destination_zone_id,
                FLD(back_channel) std::move(params.in_back_channel),
                FLD(query) std::move(params.query)});
        int ret = response_result.error_code;
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed get_schema call_peer");
            CO_RETURN get_schema_result{ret, rpc::encoding::not_set, {}, {}};
        }

        auto& response_data = response_result.payload;
        get_schema_result result;
        result.error_code = response_data.err_code;
        result.out_back_channel = std::move(response_data.back_channel);
        result.response = std::move(response_data.response);
        CO_RETURN result;
    }

    CORO_TASK(standard_result)
    transport::outbound_add_ref(add_ref_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_add_ref zone={}", get_zone_id().get_subnet());

        auto response_result = CO_AWAIT call_peer<addref_send, addref_receive>(
            params.protocol_version,
            addref_send{FLD(destination_zone_id) params.remote_object_id,
                FLD(caller_zone_id) params.caller_zone_id,
                FLD(requesting_zone_id) params.requesting_zone_id,
                FLD(request_id) params.request_id,
                FLD(build_out_param_channel) params.build_out_param_channel,
                FLD(back_channel) std::move(params.in_back_channel),
                FLD(payload) std::move(params.payload)});
        int ret = response_result.error_code;
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed add_ref addref_send");
            CO_RETURN standard_result{ret, {}};
        }

        auto& response_data = response_result.payload;
        if (response_data.err_code != rpc::error::OK())
        {
            RPC_ERROR("failed addref_receive.err_code");
#ifdef CANOPY_USE_TELEMETRY
            if (auto telemetry_service = rpc::telemetry::get_telemetry_service(); telemetry_service)
            {
                auto error_message = std::string("add_ref failed ") + std::to_string(response_data.err_code);
                telemetry_service->message({rpc::telemetry::i_telemetry_service::err, error_message.c_str()});
            }
#endif
            CO_RETURN standard_result{response_data.err_code, std::move(response_data.back_channel)};
        }

        RPC_DEBUG("stream_transport::transport::outbound_add_ref complete zone={}", get_zone_id().get_subnet());
        CO_RETURN standard_result{rpc::error::OK(), std::move(response_data.back_channel)};
    }

    CORO_TASK(standard_result)
    transport::outbound_release(release_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_release zone={}", get_zone_id().get_subnet());

        const auto status = get_status();
        if (status == rpc::transport_status::DISCONNECTED)
        {
            RPC_DEBUG(
                "skipping stream_transport::transport::outbound_release - already disconnected, zone={}",
                get_zone_id().get_subnet());
            CO_RETURN standard_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        if (status != rpc::transport_status::CONNECTED && status != rpc::transport_status::DISCONNECTING)
        {
            RPC_ERROR(
                "failed stream_transport::transport::outbound_release - not connected, status = {}",
                static_cast<int>(status));
            CO_RETURN standard_result{rpc::error::TRANSPORT_ERROR(), {}};
        }

        send_payload_release_send(
            params.protocol_version,
            message_direction::one_way,
            release_send{FLD(destination_zone_id) params.remote_object_id,
                FLD(caller_zone_id) params.caller_zone_id,
                FLD(options) params.options,
                FLD(back_channel) std::move(params.in_back_channel),
                FLD(payload) std::move(params.payload)},
            0);

        CO_RETURN standard_result{rpc::error::OK(), {}};
    }

    CORO_TASK(handshake_result)
    transport::outbound_handshake(handshake_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_handshake zone={}", get_zone_id().get_subnet());

        auto response_result = CO_AWAIT call_peer<handshake_send, handshake_receive>(
            params.protocol_version,
            handshake_send{FLD(caller_zone_id) params.caller_zone_id,
                FLD(destination_zone_id) params.destination_zone_id,
                FLD(type_id) params.type_id,
                FLD(payload_encoding) params.payload_encoding,
                FLD(payload) std::move(params.payload),
                FLD(back_channel) std::move(params.in_back_channel)});
        if (response_result.error_code != rpc::error::OK())
        {
            auto result = handshake_result{rpc::error::sanitise_public_control_status(
                                               response_result.error_code, "stream handshake transport carrier"),
                0,
                {},
                {}};
            CO_RETURN result;
        }

        auto& response = response_result.payload;
        response.err_code = rpc::error::sanitise_public_control_status(response.err_code, "stream handshake response");
        auto result = handshake_result{
            response.err_code, response.type_id, std::move(response.payload), std::move(response.back_channel)};
        CO_RETURN result;
    }

    CORO_TASK(void)
    transport::outbound_object_released(object_released_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_object_released zone={}", get_zone_id().get_subnet());

        if (get_status() == rpc::transport_status::DISCONNECTED)
        {
            RPC_ERROR("failed stream_transport::transport::outbound_object_released - transport disconnected");
            CO_RETURN;
        }

        send_payload_object_released_send(
            params.protocol_version,
            message_direction::one_way,
            object_released_send{FLD(destination_zone_id) params.remote_object_id,
                FLD(caller_zone_id) params.caller_zone_id,
                FLD(back_channel) std::move(params.in_back_channel),
                FLD(payload) std::move(params.payload)},
            0);

        RPC_DEBUG("stream_transport::transport::outbound_object_released complete zone={}", get_zone_id().get_subnet());
    }

    CORO_TASK(void)
    transport::outbound_transport_down(transport_down_params params)
    {
        RPC_DEBUG("stream_transport::transport::outbound_transport_down zone={}", get_zone_id().get_subnet());

        if (get_status() == rpc::transport_status::DISCONNECTED)
        {
            RPC_ERROR("failed stream_transport::transport::outbound_transport_down - transport disconnected");
            CO_RETURN;
        }

        send_payload_transport_down_send(
            params.protocol_version,
            message_direction::one_way,
            transport_down_send{FLD(destination_zone_id) params.destination_zone_id,
                FLD(caller_zone_id) params.caller_zone_id,
                FLD(back_channel) std::move(params.in_back_channel),
                FLD(payload) std::move(params.payload)},
            0);

        RPC_DEBUG("stream_transport::transport::outbound_transport_down complete zone={}", get_zone_id().get_subnet());
    }

    CORO_TASK(void)
    transport::outbound_post_report(rpc::telemetry_event event)
    {
        if (get_status() >= rpc::transport_status::DISCONNECTING)
            CO_RETURN;

        send_payload(rpc::get_version(), message_direction::one_way, std::move(event), 0, send_priority::high);
        CO_RETURN;
    }

    CORO_TASK(new_zone_id_result)
    transport::outbound_get_new_zone_id(get_new_zone_id_params params)
    {
        if (get_status() != rpc::transport_status::CONNECTED)
            CO_RETURN new_zone_id_result{rpc::error::TRANSPORT_ERROR(), {}, {}};

        auto result = CO_AWAIT call_peer<get_new_zone_id_send, get_new_zone_id_receive>(
            params.protocol_version, get_new_zone_id_send{FLD(back_channel) std::move(params.in_back_channel)});
        if (result.error_code != rpc::error::OK())
        {
            CO_RETURN new_zone_id_result{
                rpc::error::sanitise_public_control_status(result.error_code, "stream get_new_zone_id transport carrier"),
                {},
                {}};
        }

        result.payload.err_code
            = rpc::error::sanitise_public_control_status(result.payload.err_code, "stream get_new_zone_id response");
        if (result.payload.err_code != rpc::error::OK())
        {
            result.payload.zone_id = {};
            result.payload.back_channel.clear();
        }
        CO_RETURN new_zone_id_result{
            result.payload.err_code, result.payload.zone_id, std::move(result.payload.back_channel)};
    }

    CORO_TASK(transport::message_hook_result)
    transport::run_custom_message_handlers(message_handler_context context)
    {
        for (auto& handler : custom_message_handlers_)
        {
            auto result = CO_AWAIT handler(context);
            if (result != message_hook_result::unhandled)
                CO_RETURN result;
        }

        CO_RETURN message_hook_result::unhandled;
    }

    CORO_TASK(bool)
    transport::dispatch_builtin_message(message_handler_context context)
    {
        auto& prefix = *context.prefix;
        auto& payload = *context.payload;
        auto tracker = std::move(context.tracker);
        if (!tracker->svc)
            CO_RETURN false;

        if (payload.payload_fingerprint == rpc::id<init_client_channel_send>::get(prefix.version))
        {
            if (!tracker->svc->SPAWN(create_stub(tracker, prefix, std::move(payload))))
            {
                RPC_ERROR("dispatch_builtin_message: SPAWN(create_stub) failed — disconnecting");
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN false;
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<call_send>::get(prefix.version))
        {
            if (!tracker->svc->SPAWN(stub_handle_send(tracker, prefix, std::move(payload))))
            {
                RPC_ERROR("dispatch_builtin_message: SPAWN(stub_handle_send(tracker, prefix, std::move(payload))) failed — disconnecting");
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN false;
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<post_send>::get(prefix.version))
        {
            if (!tracker->svc->SPAWN(stub_handle_post(tracker, prefix, std::move(payload))))
            {
                RPC_ERROR("dispatch_builtin_message: SPAWN(stub_handle_post(tracker, prefix, std::move(payload))) failed — disconnecting");
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN false;
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<try_cast_send>::get(prefix.version))
        {
            if (!tracker->svc->SPAWN(stub_handle_try_cast(tracker, prefix, std::move(payload))))
            {
                RPC_ERROR("dispatch_builtin_message: SPAWN(stub_handle_try_cast(tracker, prefix, std::move(payload))) failed — disconnecting");
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN false;
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<get_schema_send>::get(prefix.version))
        {
            if (!tracker->svc->SPAWN(stub_handle_get_schema(tracker, prefix, std::move(payload))))
            {
                RPC_ERROR("dispatch_builtin_message: SPAWN(stub_handle_get_schema(tracker, prefix, std::move(payload))) failed — disconnecting");
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN false;
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<addref_send>::get(prefix.version))
        {
            if (!tracker->svc->SPAWN(stub_handle_add_ref(tracker, prefix, std::move(payload))))
            {
                RPC_ERROR("dispatch_builtin_message: SPAWN(stub_handle_add_ref(tracker, prefix, std::move(payload))) failed — disconnecting");
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN false;
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<release_send>::get(prefix.version))
        {
            if (!tracker->svc->SPAWN(stub_handle_release(tracker, prefix, std::move(payload))))
            {
                RPC_ERROR("dispatch_builtin_message: SPAWN(stub_handle_release(tracker, prefix, std::move(payload))) failed — disconnecting");
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN false;
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<handshake_send>::get(prefix.version))
        {
            if (!tracker->svc->SPAWN(stub_handle_handshake(tracker, prefix, std::move(payload))))
            {
                RPC_ERROR("dispatch_builtin_message: SPAWN(stub_handle_handshake(tracker, prefix, std::move(payload))) failed — disconnecting");
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN false;
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<object_released_send>::get(prefix.version))
        {
            if (!tracker->svc->SPAWN(stub_handle_object_released(tracker, prefix, std::move(payload))))
            {
                RPC_ERROR("dispatch_builtin_message: SPAWN(stub_handle_object_released(tracker, prefix, std::move(payload))) failed — disconnecting");
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN false;
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<transport_down_send>::get(prefix.version))
        {
            if (!tracker->svc->SPAWN(stub_handle_transport_down(tracker, prefix, std::move(payload))))
            {
                RPC_ERROR("dispatch_builtin_message: SPAWN(stub_handle_transport_down(tracker, prefix, std::move(payload))) failed — disconnecting");
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN false;
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<get_new_zone_id_send>::get(prefix.version))
        {
            if (!tracker->svc->SPAWN(stub_handle_get_new_zone_id(tracker, prefix, std::move(payload))))
            {
                RPC_ERROR("dispatch_builtin_message: SPAWN(stub_handle_get_new_zone_id(tracker, prefix, std::move(payload))) failed — disconnecting");
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN false;
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<rpc::telemetry_event>::get(prefix.version))
        {
            if (!tracker->svc->SPAWN(stub_handle_post_report(tracker, prefix, std::move(payload))))
            {
                RPC_ERROR("dispatch_builtin_message: SPAWN(stub_handle_post_report(tracker, prefix, std::move(payload))) failed — disconnecting");
                set_status(rpc::transport_status::DISCONNECTING);
                CO_RETURN false;
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<init_client_initial_channel_response>::get(prefix.version))
        {
            init_client_initial_channel_response early_response;
            auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), early_response);
            if (str_err.empty())
            {
                RPC_DEBUG(
                    "pump: received init_client_initial_channel_response, adjacent_zone={}",
                    early_response.zone_id.get_subnet());
                set_adjacent_zone_id(early_response.zone_id);
                if (registers_with_service_on_initial_channel_response())
                    tracker->svc->add_transport(early_response.zone_id, shared_from_this());

                if (stub_)
                {
                    auto stub = std::move(stub_);
                    stub_.reset();
                    if (!tracker->svc->SPAWN(handle_initial_stub_add_ref(std::move(stub), early_response.zone_id)))
                    {
                        RPC_ERROR("dispatch_builtin_message: SPAWN(handle_initial_stub_add_ref) failed — disconnecting");
                        set_status(rpc::transport_status::DISCONNECTING);
                        CO_RETURN false;
                    }
                }
            }
            else
            {
                RPC_ERROR("failed to deserialize init_client_initial_channel_response");
            }
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<close_connection_send>::get(prefix.version))
        {
            RPC_DEBUG(
                "stream dispatch close_connection_send zone={} adjacent={} seq={}",
                get_zone_id().get_subnet(),
                get_adjacent_zone_id().get_subnet(),
                prefix.sequence_number);
            set_status(rpc::transport_status::DISCONNECTING);
            peer_requested_disconnection_ = true;
            CO_RETURN true;
        }
        else if (payload.payload_fingerprint == rpc::id<close_connection_ack>::get(prefix.version))
        {
            RPC_DEBUG(
                "stream dispatch close_connection_ack zone={} adjacent={} seq={}",
                get_zone_id().get_subnet(),
                get_adjacent_zone_id().get_subnet(),
                prefix.sequence_number);
            set_status(rpc::transport_status::DISCONNECTED);
            CO_RETURN true;
        }

        CO_RETURN false;
    }

    CORO_TASK(void)
    transport::handle_initial_stub_add_ref(
        std::shared_ptr<rpc::object_stub> stub,
        rpc::zone adjacent_zone_id)
    {
        if (!stub)
            CO_RETURN;

        auto ret = CO_AWAIT stub->add_ref(false, false, adjacent_zone_id);
        if (ret != rpc::error::OK())
            set_status(transport_status::DISCONNECTING);
        CO_RETURN;
    }

    CORO_TASK(void) transport::pump_send_and_receive()
    {
        auto self = shared_from_this();

        bool expected = false;
        if (!pumps_started_.compare_exchange_strong(expected, true))
        {
            RPC_ERROR("pump_send_and_receive called MULTIPLE TIMES on zone {} - BUG!", get_zone_id().get_subnet());
            CO_RETURN;
        }

        auto svc = get_service();
        auto tracker = std::shared_ptr<activity_tracker>(new activity_tracker{
            FLD(transport) std::static_pointer_cast<rpc::stream_transport::transport>(self), FLD(svc) svc});
        // The receive and send pumps are the transport's lifeline. If either
        // fails to spawn (no executor configured, or executor already shut
        // down) the transport cannot make progress — set DISCONNECTING so
        // call_peer() callers see a clean failure rather than blocking
        // forever waiting for a reply that will never arrive.
        bool receive_spawned = svc->SPAWN(receive_consumer_loop(tracker));
        bool send_spawned = svc->SPAWN(send_producer_loop(tracker));
        if (!receive_spawned || !send_spawned)
        {
            RPC_ERROR(
                "pump_send_and_receive: failed to spawn pumps (recv={}, send={}) for zone {} — disconnecting",
                receive_spawned,
                send_spawned,
                get_zone_id().get_subnet());
            set_status(rpc::transport_status::DISCONNECTING);
            bool should_cleanup = false;
            if (!receive_spawned)
                should_cleanup = tracker->io_loop_done() || should_cleanup;
            if (!send_spawned)
                should_cleanup = tracker->io_loop_done() || should_cleanup;
            if (should_cleanup)
                CO_AWAIT cleanup(tracker->transport, tracker->svc);
            CO_RETURN;
        }
        if (call_timeout_.count() > 0 && call_timeout_sweep_.count() > 0)
        {
            if (!svc->SPAWN(timeout_sweep_loop(std::static_pointer_cast<rpc::stream_transport::transport>(self), svc)))
            {
                // Timeout sweeper failure is recoverable — call_peer() will
                // still complete normally, just without enforcement of
                // call_timeout_. Log and continue.
                RPC_WARNING(
                    "pump_send_and_receive: failed to spawn timeout sweeper for zone {} — calls will not time out",
                    get_zone_id().get_subnet());
            }
        }
    }

    CORO_TASK(void)
    transport::receive_consumer_loop(std::shared_ptr<activity_tracker> tracker)
    {
        auto self = shared_from_this();
        auto svc = get_service();
        RPC_ASSERT(svc);
        static auto envelope_prefix_saved_size = rpc::yas_binary_saved_size(envelope_prefix());

        std::vector<char> prefix_buf(envelope_prefix_saved_size);
        std::vector<char> payload_buf;

        bool receiving_prefix = true;
        rpc::mutable_byte_span remaining;

        envelope_prefix prefix{};

        bool stop_loop = false;
        while (get_status() < rpc::transport_status::DISCONNECTED && !stop_loop)
        {
            if (receiving_prefix && remaining.empty())
            {
                remaining = prefix_buf;
            }

            auto [recv_status, recv_bytes] = CO_AWAIT stream_->receive(remaining, std::chrono::milliseconds{1});

            if (recv_status.is_closed())
            {
                break;
            }

            if (recv_status.is_timeout())
            {
                if (get_status() == rpc::transport_status::DISCONNECTING)
                {
                    if (peer_requested_disconnection_)
                    {
                        if (send_cleanup_done_.load(std::memory_order_acquire))
                        {
                            send_payload_close_connection_ack(
                                rpc::get_version(), message_direction::one_way, close_connection_ack{}, 0);
                            CO_AWAIT flush_send_queue();
                            set_status(rpc::transport_status::DISCONNECTED);
                            stop_loop = true;
                            break;
                        }

                        if (shutdown_timeout_.count() > 0
                            && (std::chrono::steady_clock::now() - disconnecting_since_) >= shutdown_timeout_)
                        {
                            RPC_WARNING(
                                "receive_consumer_loop: responder shutdown timeout for zone {}, forcing DISCONNECTED",
                                get_zone_id().get_subnet());
                            set_status(rpc::transport_status::DISCONNECTED);
                            stop_loop = true;
                            break;
                        }
                    }
                    else
                    {
                        if (shutdown_timeout_.count() > 0
                            && (std::chrono::steady_clock::now() - disconnecting_since_) >= shutdown_timeout_)
                        {
                            RPC_WARNING(
                                "receive_consumer_loop: initiator shutdown timeout for zone {}, forcing DISCONNECTED",
                                get_zone_id().get_subnet());
                            set_status(rpc::transport_status::DISCONNECTED);
                            stop_loop = true;
                            break;
                        }
                    }
                }
                CO_AWAIT svc->get_executor()->schedule();
                continue;
            }

            if (!recv_status.is_ok())
            {
                RPC_WARNING(
                    "receive_consumer_loop: stream receive failed for zone {} status={} native={}",
                    get_zone_id().get_subnet(),
                    static_cast<int>(recv_status.type),
                    recv_status.native_code);
                break;
            }

            remaining = remaining.subspan(recv_bytes.size());

            if (!remaining.empty())
                continue;

            if (receiving_prefix)
            {
                auto str_err = rpc::from_yas_binary(rpc::byte_span(prefix_buf), prefix);
                if (!str_err.empty())
                {
                    RPC_ERROR("Deserialization FAILED: {}", str_err);
                    break;
                }
                if (static_cast<uint64_t>(prefix.direction) == 0)
                {
                    RPC_ERROR("invalid envelope prefix: missing direction");
                    set_status(rpc::transport_status::DISCONNECTING);
                    break;
                }
                receiving_prefix = false;
                payload_buf.assign(prefix.payload_size, '\0');
                remaining = payload_buf;
                continue;
            }

            // Full payload received — dispatch
            {
                envelope_payload payload;
                auto str_err = rpc::from_yas_binary(rpc::byte_span(payload_buf), payload);
                if (!str_err.empty())
                {
                    RPC_ERROR("invalid envelope payload: {}", str_err);
                    set_status(rpc::transport_status::DISCONNECTING);
                    break;
                }

                auto hook_result = CO_AWAIT run_custom_message_handlers(
                    message_handler_context{FLD(tracker) tracker, FLD(prefix) & prefix, FLD(payload) & payload});
                if (hook_result == message_hook_result::handled)
                {
                    receiving_prefix = true;
                    remaining = rpc::mutable_byte_span{};
                    continue;
                }
                else if (hook_result == message_hook_result::rejected)
                {
                    RPC_WARNING("message rejected fingerprint={}", payload.payload_fingerprint);
                    set_status(rpc::transport_status::DISCONNECTING);
                    break;
                }

                if (CO_AWAIT dispatch_builtin_message(
                        message_handler_context{FLD(tracker) tracker, FLD(prefix) & prefix, FLD(payload) & payload}))
                {
                    if (get_status() == rpc::transport_status::DISCONNECTED)
                    {
                        stop_loop = true;
                    }
                }
                else
                {
                    // Find the pending result listener and wake it
                    std::shared_ptr<result_listener> result;
                    {
                        std::scoped_lock lock(pending_transmits_mtx_);
                        auto it = pending_transmits_.find(prefix.sequence_number);

                        if (it != pending_transmits_.end())
                        {
                            result = std::move(it->second);
                            pending_transmits_.erase(it);
                        }
                        else
                        {
                            RPC_WARNING(
                                "No pending transmit found for sequence_number: {}, ignoring message id: {}",
                                prefix.sequence_number,
                                payload.payload_fingerprint);
                        }
                    }

                    if (result)
                        result->complete(rpc::error::OK(), prefix, std::move(payload));
                }

                receiving_prefix = true;
                remaining = rpc::mutable_byte_span{};
            }
        }

        if (get_status() < rpc::transport_status::DISCONNECTED)
        {
            set_status(rpc::transport_status::DISCONNECTED);
        }

        if (tracker->io_loop_done())
            CO_AWAIT cleanup(tracker->transport, tracker->svc);
        CO_RETURN;
    }

    CORO_TASK(bool) transport::flush_send_queue()
    {
        while (true)
        {
            queued_send_message item;
            {
                std::scoped_lock g(send_queue_mtx_);
                if (!high_priority_send_queue_.empty())
                {
                    item = std::move(high_priority_send_queue_.front());
                    high_priority_send_queue_.pop();
                }
                else if (!normal_send_queue_.empty())
                {
                    item = std::move(normal_send_queue_.front());
                    normal_send_queue_.pop();
                }
                else
                    break;
            }
            auto send_part = [this](const std::vector<uint8_t>& data) -> CORO_TASK(rpc::io_status)
            {
                if (data.empty())
                    CO_RETURN rpc::io_status{rpc::io_status::kind::ok, 0};
                CO_RETURN CO_AWAIT stream_->send(rpc::byte_span{reinterpret_cast<const char*>(data.data()), data.size()});
            };

            auto send_status = CO_AWAIT send_part(item.prefix_data);
            if (send_status.is_ok())
                send_status = CO_AWAIT send_part(item.payload_data);
            if (!send_status.is_ok())
            {
                if (is_peer_disconnect_send_failure(send_status))
                {
                    RPC_DEBUG(
                        "flush_send_queue: peer disconnected while sending queued data for zone {} status={} native={}",
                        get_zone_id().get_subnet(),
                        static_cast<int>(send_status.type),
                        send_status.native_code);
                }
                else
                {
                    RPC_WARNING(
                        "flush_send_queue: stream send failed for zone {} status={} native={}",
                        get_zone_id().get_subnet(),
                        static_cast<int>(send_status.type),
                        send_status.native_code);
                }
                if (get_status() < rpc::transport_status::DISCONNECTED)
                    set_status(rpc::transport_status::DISCONNECTED);
                CO_RETURN false;
            }
        }
        CO_RETURN true;
    }

    CORO_TASK(void) transport::send_producer_loop(std::shared_ptr<activity_tracker> tracker)
    {
        auto self = shared_from_this();
        auto svc = get_service();
        RPC_ASSERT(svc);

        while (get_status() == rpc::transport_status::CONNECTED)
        {
            queued_send_message item;
            bool had_item = false;
            {
                std::scoped_lock g(send_queue_mtx_);
                if (!high_priority_send_queue_.empty())
                {
                    item = std::move(high_priority_send_queue_.front());
                    high_priority_send_queue_.pop();
                    had_item = true;
                }
                else if (!normal_send_queue_.empty())
                {
                    item = std::move(normal_send_queue_.front());
                    normal_send_queue_.pop();
                    had_item = true;
                }
                else if (get_status() == rpc::transport_status::CONNECTED)
                {
                    send_queue_ready_.reset();
                }
            }

            if (had_item)
            {
                auto send_part = [this](const std::vector<uint8_t>& data) -> CORO_TASK(rpc::io_status)
                {
                    if (data.empty())
                        CO_RETURN rpc::io_status{rpc::io_status::kind::ok, 0};
                    CO_RETURN CO_AWAIT stream_->send(
                        rpc::byte_span{reinterpret_cast<const char*>(data.data()), data.size()});
                };
                auto send_status = CO_AWAIT send_part(item.prefix_data);
                if (send_status.is_ok())
                {
                    send_status = CO_AWAIT send_part(item.payload_data);
                }
                if (!send_status.is_ok())
                {
                    if (is_peer_disconnect_send_failure(send_status))
                    {
                        RPC_DEBUG(
                            "send_producer_loop: peer disconnected while sending queued data for zone {} status={} "
                            "native={}",
                            get_zone_id().get_subnet(),
                            static_cast<int>(send_status.type),
                            send_status.native_code);
                    }
                    else
                    {
                        RPC_WARNING(
                            "send_producer_loop: stream send failed for zone {} status={} native={}",
                            get_zone_id().get_subnet(),
                            static_cast<int>(send_status.type),
                            send_status.native_code);
                    }
                    if (get_status() < rpc::transport_status::DISCONNECTED)
                        set_status(rpc::transport_status::DISCONNECTED);
                    break;
                }
            }
            else
            {
                if (get_status() != rpc::transport_status::CONNECTED)
                    continue;
                CO_AWAIT svc->get_executor()->schedule();
                if (get_status() != rpc::transport_status::CONNECTED)
                    continue;
                if (send_queue_ready_.is_set())
                    continue;
                CO_AWAIT send_queue_ready_.wait();
            }
        }

        // Flush any messages queued before DISCONNECTING was set
        if (get_status() == rpc::transport_status::DISCONNECTED && send_cleanup_done_.load(std::memory_order_acquire))
        {
            if (tracker->io_loop_done())
                CO_AWAIT cleanup(tracker->transport, tracker->svc);
            CO_RETURN;
        }

        if (!(CO_AWAIT flush_send_queue()))
        {
            send_cleanup_done_.store(true, std::memory_order_release);
            if (tracker->io_loop_done())
                CO_AWAIT cleanup(tracker->transport, tracker->svc);
            CO_RETURN;
        }

        // A refcount-zero shutdown is a clean close. transport_down is reserved for failed
        // transports where the peer cannot be trusted to unwind references normally.
        if (!clean_disconnect_requested_.load(std::memory_order_acquire) && !peer_requested_disconnection_)
            CO_AWAIT notify_all_destinations_of_disconnect();

        if (!peer_requested_disconnection_)
        {
            send_payload_close_connection_send(rpc::get_version(), message_direction::one_way, close_connection_send{}, 0);
        }

        if (!(CO_AWAIT flush_send_queue()))
        {
            send_cleanup_done_.store(true, std::memory_order_release);
            if (tracker->io_loop_done())
                CO_AWAIT cleanup(tracker->transport, tracker->svc);
            CO_RETURN;
        }

        send_cleanup_done_.store(true, std::memory_order_release);

        if (tracker->io_loop_done())
            CO_AWAIT cleanup(tracker->transport, tracker->svc);
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::timeout_sweep_loop(
        std::shared_ptr<rpc::stream_transport::transport> transport,
        std::shared_ptr<rpc::service> svc)
    {
        (void)transport;
        if (call_timeout_.count() <= 0 || call_timeout_sweep_.count() <= 0)
            CO_RETURN;

        RPC_ASSERT(svc);

        while (get_status() < rpc::transport_status::DISCONNECTED)
        {
            auto scheduler = svc->get_executor();
            CO_AWAIT scheduler->schedule_after(call_timeout_sweep_);
            if (get_status() >= rpc::transport_status::DISCONNECTED)
                break;

            auto now = std::chrono::steady_clock::now();
            std::vector<std::shared_ptr<result_listener>> timed_out;
            {
                std::scoped_lock lock(pending_transmits_mtx_);
                for (auto it = pending_transmits_.begin(); it != pending_transmits_.end();)
                {
                    if ((now - it->second->start_time) > call_timeout_)
                    {
                        auto elapsed_ms
                            = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second->start_time).count();
                        RPC_WARNING(
                            "RPC call timed out after {}ms, sequence_number: {}, zone: {}",
                            elapsed_ms,
                            it->first,
                            get_zone_id().get_subnet());
                        timed_out.push_back(std::move(it->second));
                        it = pending_transmits_.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            for (auto& listener : timed_out)
                listener->complete(rpc::error::CALL_TIMEOUT());
        }

        CO_RETURN;
    }

    CORO_TASK(void)
    transport::notify_disconnect_once(const std::shared_ptr<rpc::service>& svc)
    {
        bool expected = false;
        if (!disconnect_notification_sent_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            CO_RETURN;
        }

        CO_AWAIT notify_all_destinations_of_disconnect();
        if (svc && get_adjacent_zone_id().is_set())
            CO_AWAIT svc->notify_transport_down(shared_from_this(), get_adjacent_zone_id());
    }

    CORO_TASK(void)
    transport::cleanup(
        std::shared_ptr<rpc::stream_transport::transport> transport,
        std::shared_ptr<rpc::service> svc)
    {
        RPC_DEBUG("Both loops completed, finalising transport for zone {}", transport->get_zone_id().get_subnet());
        CO_AWAIT transport->notify_disconnect_once(svc);
        std::vector<std::shared_ptr<result_listener>> cancelled;
        {
            std::scoped_lock lock(transport->pending_transmits_mtx_);
            for (auto it : transport->pending_transmits_)
            {
                cancelled.push_back(std::move(it.second));
            }
            transport->pending_transmits_.clear();
        }
        for (auto& listener : cancelled)
            listener->complete(rpc::error::CALL_CANCELLED());
        transport->stream_.reset();
        transport->keep_alive_.reset();
        RPC_DEBUG("stream transport cleanup completed for zone {}", transport->get_zone_id().get_subnet());
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_send(
        std::shared_ptr<activity_tracker>,
        envelope_prefix prefix,
        envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_send");

        if (prefix.direction != message_direction::send && prefix.direction != message_direction::one_way)
        {
            RPC_ERROR("invalid call_send direction {}", static_cast<uint64_t>(prefix.direction));
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            CO_RETURN;
        }

        call_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed from_yas_binary call_send");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        auto send_result = CO_AWAIT inbound_send(
            rpc::send_params{
                FLD(protocol_version) prefix.version,
                FLD(encoding_type) request.encoding,
                FLD(tag) request.tag,
                FLD(caller_zone_id) request.caller_zone_id,
                FLD(remote_object_id) request.destination_zone_id,
                FLD(interface_id) request.interface_id,
                FLD(method_id) request.method_id,
                FLD(in_data) std::move(request.payload),
                FLD(in_back_channel) std::move(request.back_channel),
                FLD(request_id) request.request_id,
            });

        if (rpc::error::is_error(send_result.error_code))
        {
            RPC_DEBUG("inbound_send error {}", send_result.error_code);
        }

        if (prefix.direction == message_direction::one_way)
            CO_RETURN;

        send_payload_call_receive(
            prefix.version,
            message_direction::receive,
            call_receive{FLD(payload) std::move(send_result.out_buf),
                FLD(back_channel) std::move(send_result.out_back_channel),
                FLD(err_code) send_result.error_code},
            prefix.sequence_number);
        RPC_DEBUG("stub_handle_send complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_post(
        std::shared_ptr<activity_tracker>,
        envelope_prefix prefix,
        envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_post");

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            CO_RETURN;
        }

        post_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed post_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        CO_AWAIT inbound_post(
            rpc::post_params{
                FLD(protocol_version) prefix.version,
                FLD(encoding_type) request.encoding,
                FLD(tag) request.tag,
                FLD(caller_zone_id) request.caller_zone_id,
                FLD(remote_object_id) request.destination_zone_id,
                FLD(interface_id) request.interface_id,
                FLD(method_id) request.method_id,
                FLD(in_data) std::move(request.payload),
                FLD(in_back_channel) std::move(request.back_channel),
            });

        RPC_DEBUG("stub_handle_post complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_try_cast(
        std::shared_ptr<activity_tracker>,
        envelope_prefix prefix,
        envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_try_cast");

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            CO_RETURN;
        }

        try_cast_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed try_cast_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        auto tc_result = CO_AWAIT inbound_try_cast(
            rpc::try_cast_params{
                FLD(protocol_version) prefix.version,
                FLD(caller_zone_id) request.caller_zone_id,
                FLD(remote_object_id) request.destination_zone_id,
                FLD(interface_id) request.interface_id,
                FLD(in_back_channel) std::move(request.back_channel),
                FLD(payload) std::move(request.payload),
            });

        if (rpc::error::is_error(tc_result.error_code))
        {
            RPC_DEBUG("inbound_try_cast error {}", tc_result.error_code);
        }

        send_payload_try_cast_receive(
            prefix.version,
            message_direction::receive,
            try_cast_receive{FLD(back_channel) std::move(tc_result.out_back_channel), FLD(err_code) tc_result.error_code},
            prefix.sequence_number);
        RPC_DEBUG("stub_handle_try_cast complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_get_schema(
        std::shared_ptr<activity_tracker>,
        envelope_prefix prefix,
        envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_get_schema");

        if (get_status() != rpc::transport_status::CONNECTED)
        {
            CO_RETURN;
        }

        get_schema_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed get_schema_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        auto schema_result = CO_AWAIT inbound_get_schema(
            rpc::get_schema_params{
                FLD(protocol_version) prefix.version,
                FLD(caller_zone_id) request.caller_zone_id,
                FLD(destination_zone_id) request.destination_zone_id,
                FLD(in_back_channel) std::move(request.back_channel),
                FLD(query) std::move(request.query),
            });

        if (rpc::error::is_error(schema_result.error_code))
        {
            RPC_DEBUG("inbound_get_schema error {}", schema_result.error_code);
        }

        send_payload_get_schema_receive(
            prefix.version,
            message_direction::receive,
            get_schema_receive{FLD(err_code) schema_result.error_code,
                FLD(back_channel) std::move(schema_result.out_back_channel),
                FLD(response) std::move(schema_result.response)},
            prefix.sequence_number);
        RPC_DEBUG("stub_handle_get_schema complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_add_ref(
        std::shared_ptr<activity_tracker>,
        envelope_prefix prefix,
        envelope_payload payload)
    {
        if (get_status() != rpc::transport_status::CONNECTED)
        {
            CO_RETURN;
        }

        addref_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed addref_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        auto ar_result = CO_AWAIT inbound_add_ref(
            rpc::add_ref_params{
                FLD(protocol_version) prefix.version,
                FLD(remote_object_id) request.destination_zone_id,
                FLD(caller_zone_id) request.caller_zone_id,
                FLD(requesting_zone_id) request.requesting_zone_id,
                FLD(build_out_param_channel) request.build_out_param_channel,
                FLD(in_back_channel) std::move(request.back_channel),
                FLD(request_id) request.request_id,
                FLD(payload) std::move(request.payload),
            });

        if (rpc::error::is_error(ar_result.error_code))
        {
            RPC_DEBUG("inbound_add_ref error {}", ar_result.error_code);
        }

        send_payload_addref_receive(
            prefix.version,
            message_direction::receive,
            addref_receive{FLD(back_channel) std::move(ar_result.out_back_channel), FLD(err_code) ar_result.error_code},
            prefix.sequence_number);
        RPC_DEBUG("stub_handle_add_ref complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_release(
        std::shared_ptr<activity_tracker>,
        envelope_prefix prefix,
        envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_release");

        release_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed release_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }
        auto rel_result = CO_AWAIT inbound_release(
            rpc::release_params{
                FLD(protocol_version) prefix.version,
                FLD(remote_object_id) request.destination_zone_id,
                FLD(caller_zone_id) request.caller_zone_id,
                FLD(options) request.options,
                FLD(in_back_channel) std::move(request.back_channel),
                FLD(payload) std::move(request.payload),
            });

        if (rpc::error::is_error(rel_result.error_code))
        {
            RPC_DEBUG("inbound_release error {}", rel_result.error_code);
        }

        RPC_DEBUG("stub_handle_release complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_handshake(
        std::shared_ptr<activity_tracker>,
        envelope_prefix prefix,
        envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_handshake");

        handshake_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed handshake_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        auto hs_result = CO_AWAIT inbound_handshake(
            rpc::handshake_params{FLD(protocol_version) prefix.version,
                FLD(caller_zone_id) request.caller_zone_id,
                FLD(destination_zone_id) request.destination_zone_id,
                FLD(type_id) request.type_id,
                FLD(payload_encoding) request.payload_encoding,
                FLD(payload) std::move(request.payload),
                FLD(in_back_channel) std::move(request.back_channel)});
        hs_result.error_code
            = rpc::error::sanitise_public_control_status(hs_result.error_code, "stream handshake receive");
        if (hs_result.error_code != rpc::error::OK())
        {
            hs_result.type_id = 0;
            hs_result.payload.clear();
            hs_result.out_back_channel.clear();
        }
        send_payload_handshake_receive(
            prefix.version,
            message_direction::receive,
            handshake_receive{FLD(err_code) hs_result.error_code,
                FLD(type_id) hs_result.type_id,
                FLD(payload) std::move(hs_result.payload),
                FLD(back_channel) std::move(hs_result.out_back_channel)},
            prefix.sequence_number);
        RPC_DEBUG("stub_handle_handshake complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_get_new_zone_id(
        std::shared_ptr<activity_tracker>,
        envelope_prefix prefix,
        envelope_payload payload)
    {
        get_new_zone_id_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed get_new_zone_id_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        auto service = get_service();
        rpc::new_zone_id_result zone_result{rpc::error::ZONE_NOT_FOUND(), {}, {}};
        if (service)
        {
            zone_result = CO_AWAIT service->get_new_zone_id(
                rpc::get_new_zone_id_params{
                    FLD(protocol_version) prefix.version,
                    FLD(in_back_channel) std::move(request.back_channel),
                });
        }

        zone_result.error_code
            = rpc::error::sanitise_public_control_status(zone_result.error_code, "stream get_new_zone_id receive");
        if (zone_result.error_code != rpc::error::OK())
        {
            zone_result.zone_id = {};
            zone_result.out_back_channel.clear();
        }

        send_payload(
            prefix.version,
            message_direction::receive,
            get_new_zone_id_receive{FLD(err_code) zone_result.error_code,
                FLD(zone_id) zone_result.zone_id,
                FLD(back_channel) std::move(zone_result.out_back_channel)},
            prefix.sequence_number,
            send_priority::high);
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_object_released(
        std::shared_ptr<activity_tracker>,
        envelope_prefix prefix,
        envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_object_released");

        object_released_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed object_released_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        CO_AWAIT inbound_object_released(
            rpc::object_released_params{
                FLD(protocol_version) prefix.version,
                FLD(remote_object_id) request.destination_zone_id,
                FLD(caller_zone_id) request.caller_zone_id,
                FLD(in_back_channel) std::move(request.back_channel),
                FLD(payload) std::move(request.payload),
            });

        RPC_DEBUG("stub_handle_object_released complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_transport_down(
        std::shared_ptr<activity_tracker>,
        envelope_prefix prefix,
        envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_transport_down");

        transport_down_send request;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!str_err.empty())
        {
            RPC_ERROR("failed transport_down_send from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        CO_AWAIT inbound_transport_down(
            rpc::transport_down_params{
                FLD(protocol_version) prefix.version,
                FLD(destination_zone_id) request.destination_zone_id,
                FLD(caller_zone_id) request.caller_zone_id,
                FLD(in_back_channel) std::move(request.back_channel),
                FLD(payload) std::move(request.payload),
            });

        RPC_DEBUG("stub_handle_transport_down complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::stub_handle_post_report(
        std::shared_ptr<activity_tracker>,
        envelope_prefix prefix,
        envelope_payload payload)
    {
        RPC_DEBUG("stub_handle_post_report");

        if (prefix.direction != message_direction::one_way)
        {
            RPC_ERROR("failed rpc::telemetry_event: expected one-way message");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        rpc::telemetry_event event;
        auto str_err = rpc::from_yas_binary(rpc::byte_span(payload.payload), event);
        if (!str_err.empty())
        {
            RPC_ERROR("failed rpc::telemetry_event from_yas_binary");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        CO_AWAIT inbound_post_report(std::move(event));

        RPC_DEBUG("stub_handle_post_report complete");
        CO_RETURN;
    }

    CORO_TASK(void)
    transport::create_stub(
        std::shared_ptr<activity_tracker>,
        envelope_prefix prefix,
        envelope_payload payload)
    {
        init_client_channel_send request;
        auto err = rpc::from_yas_binary(rpc::byte_span(payload.payload), request);
        if (!err.empty())
        {
            RPC_ERROR("failed create_stub init_client_channel_send deserialization");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        rpc::connection_settings input_descr;
        input_descr.inbound_interface_id = request.inbound_interface_id;
        input_descr.outbound_interface_id = request.outbound_interface_id;
        input_descr.encoding_type = request.encoding_type;
        input_descr.remote_object_id = request.inbound_remote_object;
        if (input_descr.encoding_type == rpc::encoding::not_set)
        {
            RPC_ERROR("init_client_channel_send missing connection encoding");
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }
        set_adjacent_zone_id(request.adjacent_zone_id);

        rpc::transport_keep_alive handshake_keep_alive(shared_from_this(), input_descr.remote_object_id.as_zone());

        // Immediately inform the peer of our zone_id before invoking connection_handler_
        send_payload_init_client_initial_channel_response(
            prefix.version, message_direction::one_way, init_client_initial_channel_response{FLD(zone_id) get_zone_id()}, 0);

        auto connect_result = CO_AWAIT connection_handler_(input_descr, get_service(), keep_alive_.get_nullable());
        connection_handler_ = nullptr;
        auto& output_interface = connect_result.output_descriptor;
        int ret = connect_result.error_code;
        if (ret != rpc::error::OK())
        {
            RPC_ERROR("failed to connect to zone {}", ret);
            set_status(rpc::transport_status::DISCONNECTING);
            CO_RETURN;
        }

        send_payload_init_client_channel_response(
            prefix.version,
            message_direction::receive,
            init_client_channel_response{FLD(err_code) rpc::error::OK(),
                FLD(outbound_remote_object) output_interface,
                FLD(caller_zone_id) input_descr.remote_object_id.as_zone()},
            prefix.sequence_number);

        CO_RETURN;
    }

    void transport::send_payload_post_send(
        uint64_t protocol_version,
        message_direction direction,
        post_send&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number);
    }

    void transport::send_payload_release_send(
        uint64_t protocol_version,
        message_direction direction,
        release_send&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number);
    }

    void transport::send_payload_object_released_send(
        uint64_t protocol_version,
        message_direction direction,
        object_released_send&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number);
    }

    void transport::send_payload_transport_down_send(
        uint64_t protocol_version,
        message_direction direction,
        transport_down_send&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number);
    }

    void transport::send_payload_close_connection_ack(
        uint64_t protocol_version,
        message_direction direction,
        close_connection_ack&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, payload, sequence_number, send_priority::high);
    }

    void transport::send_payload_close_connection_send(
        uint64_t protocol_version,
        message_direction direction,
        close_connection_send&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, payload, sequence_number, send_priority::high);
    }

    void transport::send_payload_call_receive(
        uint64_t protocol_version,
        message_direction direction,
        call_receive&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number, send_priority::high);
    }

    void transport::send_payload_try_cast_receive(
        uint64_t protocol_version,
        message_direction direction,
        try_cast_receive&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number, send_priority::high);
    }

    void transport::send_payload_get_schema_receive(
        uint64_t protocol_version,
        message_direction direction,
        get_schema_receive&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number, send_priority::high);
    }

    void transport::send_payload_addref_receive(
        uint64_t protocol_version,
        message_direction direction,
        addref_receive&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number, send_priority::high);
    }

    void transport::send_payload_handshake_receive(
        uint64_t protocol_version,
        message_direction direction,
        handshake_receive&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, std::move(payload), sequence_number, send_priority::high);
    }

    void transport::send_payload_init_client_initial_channel_response(
        uint64_t protocol_version,
        message_direction direction,
        init_client_initial_channel_response&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, payload, sequence_number, send_priority::high);
    }

    void transport::send_payload_init_client_channel_response(
        uint64_t protocol_version,
        message_direction direction,
        init_client_channel_response&& payload,
        uint64_t sequence_number)
    {
        if (run_outgoing_handlers(protocol_version, direction, sequence_number, payload))
            return;
        send_payload(protocol_version, direction, payload, sequence_number, send_priority::high);
    }
}
