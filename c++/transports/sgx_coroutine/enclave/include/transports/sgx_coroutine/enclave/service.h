/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */
#pragma once

#include <io_uring/controller.h>
#include <rpc/rpc.h>
#include <security/attestation/service.h>
#include <security/attestation/types.h>

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

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
            const char* name,
            rpc::zone zone_id,
            rpc::destination_zone parent_zone_id,
            const std::shared_ptr<rpc::coro::scheduler>& scheduler)
            : rpc::child_service(
                  name,
                  zone_id,
                  parent_zone_id,
                  scheduler)
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

        void set_attestation_service(std::shared_ptr<canopy::security::attestation::attestation_service> service);
        [[nodiscard]] auto get_attestation_service() const
            -> std::shared_ptr<canopy::security::attestation::attestation_service>;
        void set_protected_rpc_enabled(bool enabled);
        [[nodiscard]] bool protected_rpc_enabled() const;

        void add_parent_zone_proxy(const std::shared_ptr<rpc::service_proxy>& proxy) { add_zone_proxy(proxy); }

        CORO_TASK(send_result) send(send_params params) override;
        CORO_TASK(void) post(post_params params) override;
        CORO_TASK(send_result)
        outbound_send(
            send_params params,
            std::shared_ptr<transport> transport) override;
        CORO_TASK(void)
        outbound_post(
            post_params params,
            std::shared_ptr<transport> transport) override;

    private:
        [[nodiscard]] auto find_security_context_for_protected_call(
            rpc::caller_zone caller_zone_id,
            rpc::destination_zone destination_zone_id) const
            -> std::optional<canopy::security::attestation::security_context>;

        mutable std::mutex controller_mutex_;
        std::shared_ptr<rpc::io_uring::controller> io_uring_controller_;

        mutable std::mutex security_context_mutex_;
        std::unordered_map<rpc::destination_zone, canopy::security::attestation::security_context> security_contexts_;

        mutable std::mutex attestation_service_mutex_;
        std::shared_ptr<canopy::security::attestation::attestation_service> attestation_service_;
        bool protected_rpc_enabled_{false};
    };
}
