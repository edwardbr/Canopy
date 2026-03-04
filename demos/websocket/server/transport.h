/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <functional>
#include <queue>
#include <vector>
#include <mutex>
#include <atomic>

#include <rpc/rpc.h>
#include <wslay/wslay.h>
#include "websocket_demo/websocket_demo.h"

namespace websocket_demo
{
    namespace v1
    {
        // Transport from child zone to parent zone
        // Used by child to communicate with parent
        class transport : public rpc::transport
        {

        public:
            transport(std::string name, std::shared_ptr<rpc::service> service);
            transport(std::string name, rpc::zone zone_id);
            transport(wslay_event_context_ptr wslay_ctx,
                std::shared_ptr<rpc::service> service,
                const std::shared_ptr<std::queue<std::vector<uint8_t>>>& pending_messages,
                const std::shared_ptr<std::mutex>& pending_messages_mutex);

            virtual ~transport() CANOPY_DEFAULT_DESTRUCTOR;

            CORO_TASK(int)
            inner_connect(const std::shared_ptr<rpc::object_stub>& stub,
                rpc::connection_settings& input_descr,
                rpc::interface_descriptor& output_descr) override
            {
                auto svc = get_service();
                rpc::zone adjacent_zone_id;
                // get a new adjacenct_zone_id
                {
                    std::vector<rpc::back_channel_entry> out_back_channel;
                    auto err = CO_AWAIT svc->get_new_zone_id(rpc::get_version(), adjacent_zone_id, {}, out_back_channel);
                    if (err != rpc::error::OK())
                    {
                        RPC_ERROR("[WS] get_new_zone_id failed: {}", err);
                        RPC_ASSERT(false);
                        CO_RETURN err;
                    }

                    set_adjacent_zone_id(adjacent_zone_id);
                }

                svc->add_transport(adjacent_zone_id.as_destination(), shared_from_this());

                if (stub)
                {
                    auto ret = CO_AWAIT stub->add_ref(false, false, adjacent_zone_id.as_caller());
                    if (ret != rpc::error::OK())
                    {
                        CO_RETURN ret;
                    }
                }
                std::ignore = input_descr;
                std::ignore = output_descr;
                // Parent transport is connected immediately - no handshake needed
                CO_RETURN rpc::error::OK();
            }
            CORO_TASK(int) inner_accept() override { CO_RETURN rpc::error::OK(); }

            template<class in_param_type, class out_param_type>
            static std::function<CORO_TASK(int)(
                rpc::interface_descriptor input_descr, rpc::interface_descriptor& output_descr, std::shared_ptr<transport>& child)>
            bind(rpc::zone new_zone_id,
                std::function<CORO_TASK(int)(const rpc::shared_ptr<in_param_type>&,
                    rpc::shared_ptr<out_param_type>&,
                    const std::shared_ptr<rpc::child_service>&)>&& child_entry_point_fn);

            // Outbound i_marshaller interface - sends from child to parent
            CORO_TASK(int)
            outbound_send(uint64_t protocol_version,
                rpc::encoding encoding,
                uint64_t tag,
                rpc::caller_zone caller_zone_id,
                rpc::remote_object remote_object_id,
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
                rpc::remote_object remote_object_id,
                rpc::interface_ordinal interface_id,
                rpc::method method_id,
                const rpc::span& in_data,
                const std::vector<rpc::back_channel_entry>& back_channel) override;

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
                const std::vector<rpc::back_channel_entry>& back_channel,
                std::vector<rpc::back_channel_entry>& out_back_channel) override;

            CORO_TASK(int)
            outbound_release(uint64_t protocol_version,
                rpc::remote_object remote_object_id,
                rpc::caller_zone caller_zone_id,
                rpc::release_options options,
                const std::vector<rpc::back_channel_entry>& back_channel,
                std::vector<rpc::back_channel_entry>& out_back_channel) override;

            // New methods from i_marshaller interface
            CORO_TASK(void)
            outbound_object_released(uint64_t protocol_version,
                rpc::remote_object remote_object_id,
                rpc::caller_zone caller_zone_id,
                const std::vector<rpc::back_channel_entry>& back_channel) override;

            CORO_TASK(void)
            outbound_transport_down(uint64_t protocol_version,
                rpc::destination_zone destination_zone_id,
                rpc::caller_zone caller_zone_id,
                const std::vector<rpc::back_channel_entry>& back_channel) override;

            CORO_TASK(void) stub_handle_send(websocket_demo::v1::envelope request);

            void set_local_object_id(rpc::object id) { local_object_id_ = id; }

        private:
            wslay_event_context_ptr wslay_ctx_{nullptr};
            std::shared_ptr<std::queue<std::vector<uint8_t>>> pending_messages_;
            std::shared_ptr<std::mutex> pending_messages_mutex_;
            rpc::object local_object_id_{0};
        };
    }
}
