/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#pragma once

#include <memory>

#include <security/attestation/service.h>
#include <security/attestation/types.h>

namespace canopy::security::attestation
{
    enum class configured_backend_kind
    {
        null_backend,
        fake_backend,
        sgx_sim_backend,
        sgx_epid_backend,
        sgx_dcap_backend
    };

    [[nodiscard]] auto attestation_backend_kind_name(configured_backend_kind kind) noexcept -> const char*;
    [[nodiscard]] auto configured_attestation_backend_kind() noexcept -> configured_backend_kind;
    [[nodiscard]] auto configured_attestation_backend_name() noexcept -> const char*;
    [[nodiscard]] auto make_attestation_backend(configured_backend_kind kind) -> std::shared_ptr<attestation_backend>;
    [[nodiscard]] auto make_configured_attestation_backend() -> std::shared_ptr<attestation_backend>;
    [[nodiscard]] auto make_configured_attestation_service_options(identity local_identity) -> attestation_service_options;
} // namespace canopy::security::attestation
