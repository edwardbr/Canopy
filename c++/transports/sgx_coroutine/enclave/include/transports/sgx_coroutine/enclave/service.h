/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <io_uring/controller.h>
#include <rpc/rpc.h>
#include <security/attestation/service.h>
#include <security/attestation/types.h>
#include <security/attestation/zone_security_policy.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace streaming
{
    class stream;
}

namespace rpc
{
    class enclave_service : public rpc::child_service
    {
    public:
        enclave_service(
            std::string name,
            rpc::zone zone_id,
            rpc::destination_zone parent_zone_id,
            const rpc::executor_ptr& executor)
            : rpc::child_service(
                  std::move(name),
                  zone_id,
                  parent_zone_id,
                  executor)
            , zone_security_policy_(std::make_shared<canopy::security::attestation::zone_security_policy>())
        {
        }

        ~enclave_service() override;

        void set_io_uring_controller(std::shared_ptr<rpc::io_uring::controller> controller)
        {
            std::lock_guard<std::mutex> lock(controller_mutex_);
            io_uring_controller_ = controller;
        }

        [[nodiscard]] std::shared_ptr<rpc::io_uring::controller> get_io_uring_controller() const
        {
            std::lock_guard<std::mutex> lock(controller_mutex_);
            return io_uring_controller_;
        }

        void set_security_context(
            rpc::destination_zone attested_zone_id,
            canopy::security::attestation::security_context context);
        [[nodiscard]] bool publish_security_context_from_stream(
            rpc::destination_zone attested_zone_id,
            const std::shared_ptr<streaming::stream>& stream);
        void remove_security_context(rpc::destination_zone attested_zone_id);
        [[nodiscard]] auto get_security_context(rpc::destination_zone attested_zone_id) const
            -> std::optional<canopy::security::attestation::security_context>;
        void set_attestation_route_state(
            rpc::destination_zone attested_zone_id,
            canopy::security::attestation::route_attestation_state state);
        [[nodiscard]] auto get_attestation_route_state(rpc::destination_zone attested_zone_id) const
            -> canopy::security::attestation::route_attestation_state;

        void set_attestation_service(std::shared_ptr<canopy::security::attestation::attestation_service> service);
        [[nodiscard]] auto get_attestation_service() const
            -> std::shared_ptr<canopy::security::attestation::attestation_service>;
        void set_zone_security_policy(std::shared_ptr<canopy::security::attestation::zone_security_policy> policy);
        [[nodiscard]] auto get_zone_security_policy() const
            -> std::shared_ptr<canopy::security::attestation::zone_security_policy>;
        void set_protected_rpc_enabled(bool enabled);
        [[nodiscard]] bool protected_rpc_enabled() const;
        void set_add_ref_attestation_required(bool required);
        [[nodiscard]] bool add_ref_attestation_required() const;
        void set_route_unattested_allowed(
            rpc::destination_zone route_zone_id,
            bool allowed);

        std::shared_ptr<rpc::service_proxy> add_parent_zone_proxy(const std::shared_ptr<rpc::service_proxy>& proxy)
        {
            return add_zone_proxy(proxy);
        }

        CORO_TASK(send_result) send(send_params params) override;
        CORO_TASK(void) post(post_params params) override;
        CORO_TASK(standard_result) try_cast(try_cast_params params) override;
        CORO_TASK(standard_result) add_ref(add_ref_params params) override;
        CORO_TASK(standard_result) release(release_params params) override;
        CORO_TASK(void) object_released(object_released_params params) override;
        CORO_TASK(void) transport_down(transport_down_params params) override;
        CORO_TASK(handshake_result) handshake(handshake_params params) override;
        CORO_TASK(send_result)
        outbound_send(
            send_params params,
            std::shared_ptr<transport> transport) override;
        CORO_TASK(void)
        outbound_post(
            post_params params,
            std::shared_ptr<transport> transport) override;
        CORO_TASK(standard_result)
        outbound_try_cast(
            try_cast_params params,
            std::shared_ptr<transport> transport) override;
        CORO_TASK(standard_result)
        outbound_add_ref(
            add_ref_params params,
            std::shared_ptr<transport> transport) override;
        CORO_TASK(standard_result)
        outbound_release(
            release_params params,
            std::shared_ptr<transport> transport) override;
        CORO_TASK(void)
        outbound_object_released(
            object_released_params params,
            std::shared_ptr<transport> transport) override;

    private:
        struct route_attestation_claim
        {
            canopy::security::attestation::route_policy_decision decision;
            canopy::security::attestation::route_attestation_state state;
            uint64_t transcript_id{0};
            int error_code{rpc::error::OK()};
        };

        [[nodiscard]] auto claim_add_ref_route_attestation(
            rpc::destination_zone route_zone_id,
            bool route_is_local,
            bool attestation_required) -> route_attestation_claim;
        [[nodiscard]] auto fail_claimed_attestation_route(
            rpc::destination_zone route_zone_id,
            uint64_t transcript_id,
            uint64_t previous_failure_epoch,
            std::string reason) -> bool;
        [[nodiscard]] auto complete_claimed_attestation_route(
            rpc::destination_zone route_zone_id,
            uint64_t transcript_id,
            canopy::security::attestation::security_context context) -> bool;
        [[nodiscard]] auto complete_claimed_unattested_route(
            rpc::destination_zone route_zone_id,
            uint64_t transcript_id) -> bool;
        [[nodiscard]] auto record_inbound_attestation_failure(
            rpc::destination_zone route_zone_id,
            std::string reason) -> bool;
        [[nodiscard]] auto complete_inbound_attestation_route(
            rpc::destination_zone route_zone_id,
            canopy::security::attestation::security_context context) -> bool;
        [[nodiscard]] auto complete_inbound_unattested_route(rpc::destination_zone route_zone_id) -> bool;
        [[nodiscard]] auto result_for_superseded_add_ref_claim(
            rpc::destination_zone route_zone_id,
            const char* operation,
            int fallback_error_code) const -> rpc::standard_result;
        [[nodiscard]] auto find_security_context_for_protected_call(
            rpc::caller_zone caller_zone_id,
            rpc::destination_zone destination_zone_id) const
            -> std::optional<canopy::security::attestation::security_context>;
        CORO_TASK(standard_result)
        ensure_add_ref_route_allowed(
            rpc::destination_zone route_zone_id,
            const char* operation);
        CORO_TASK(standard_result)
        ensure_existing_reference_route_allowed(
            rpc::destination_zone route_zone_id,
            const char* operation) const;

        mutable std::mutex controller_mutex_;
        std::shared_ptr<rpc::io_uring::controller> io_uring_controller_;

        mutable std::mutex security_context_mutex_;
        std::unordered_map<rpc::destination_zone, canopy::security::attestation::route_attestation_state> attestation_route_states_;

        mutable std::mutex attestation_service_mutex_;
        std::shared_ptr<canopy::security::attestation::attestation_service> attestation_service_;
        bool protected_rpc_enabled_{false};
        mutable std::mutex zone_security_policy_mutex_;
        std::shared_ptr<canopy::security::attestation::zone_security_policy> zone_security_policy_;
    };
}
