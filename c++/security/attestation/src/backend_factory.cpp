/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/backend_factory.h>

#include <security/attestation/fake_backend.h>
#include <security/attestation/null_backend.h>
#include <security/attestation/sgx_epid_backend.h>
#include <security/attestation/simulation_backend.h>

#include <exception>
#include <memory>
#include <utility>

namespace canopy::security::attestation
{
    auto attestation_backend_kind_name(configured_backend_kind kind) noexcept -> const char*
    {
        switch (kind)
        {
        case configured_backend_kind::null_backend:
            return null_backend_id;
        case configured_backend_kind::fake_backend:
            return fake_backend_id;
        case configured_backend_kind::sgx_sim_backend:
            return simulation_backend_id;
        case configured_backend_kind::sgx_epid_backend:
            return sgx_epid_backend_id;
        }
        std::terminate();
    }

    auto configured_attestation_backend_kind() noexcept -> configured_backend_kind
    {
#ifdef CANOPY_ATTESTATION_BACKEND_NULL
        return configured_backend_kind::null_backend;
#elif defined(CANOPY_ATTESTATION_BACKEND_SGX_SIM)
        return configured_backend_kind::sgx_sim_backend;
#elif defined(CANOPY_ATTESTATION_BACKEND_SGX_EPID)
        return configured_backend_kind::sgx_epid_backend;
#else
        return configured_backend_kind::fake_backend;
#endif
    }

    auto configured_attestation_backend_name() noexcept -> const char*
    {
        return attestation_backend_kind_name(configured_attestation_backend_kind());
    }

    auto make_attestation_backend(configured_backend_kind kind) -> std::shared_ptr<attestation_backend>
    {
        switch (kind)
        {
        case configured_backend_kind::null_backend:
            return std::make_shared<null_backend>();
        case configured_backend_kind::fake_backend:
            return std::make_shared<fake_backend>();
        case configured_backend_kind::sgx_sim_backend:
            return std::make_shared<simulation_backend>();
        case configured_backend_kind::sgx_epid_backend:
            return std::make_shared<sgx_epid_backend>();
        }
        std::terminate();
    }

    auto make_configured_attestation_backend() -> std::shared_ptr<attestation_backend>
    {
        return make_attestation_backend(configured_attestation_backend_kind());
    }

    auto make_configured_attestation_service_options(identity local_identity) -> attestation_service_options
    {
        auto kind = configured_attestation_backend_kind();

        attestation_service_options options;
        options.local_identity = std::move(local_identity);
        options.backend = make_attestation_backend(kind);

        options.policy = attestation_policy{};
        options.policy.required_backend_id = options.backend->backend_id();
        switch (kind)
        {
        case configured_backend_kind::null_backend:
            options.policy.send_local_evidence = false;
            options.policy.require_peer_evidence = false;
            options.policy.allow_unattested_peer = true;
            options.policy.allow_development_evidence = false;
            options.policy.minimum_security_level = security_level::none;
            break;
        case configured_backend_kind::fake_backend:
            options.policy.send_local_evidence = true;
            options.policy.require_peer_evidence = true;
            options.policy.allow_unattested_peer = false;
            options.policy.allow_development_evidence = true;
            options.policy.minimum_security_level = security_level::development;
            break;
        case configured_backend_kind::sgx_sim_backend:
            options.policy.send_local_evidence = true;
            options.policy.require_peer_evidence = true;
            options.policy.allow_unattested_peer = false;
            options.policy.allow_development_evidence = true;
            options.policy.minimum_security_level = security_level::simulation;
            break;
        case configured_backend_kind::sgx_epid_backend:
            options.policy.send_local_evidence = true;
            options.policy.require_peer_evidence = true;
            options.policy.allow_unattested_peer = false;
            options.policy.allow_development_evidence = false;
            options.policy.minimum_security_level = security_level::hardware_legacy;
            break;
        }
        return options;
    }
} // namespace canopy::security::attestation
