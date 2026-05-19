/*
 *   Copyright (c) 2026 Edward Boggis-Rolfe
 *   All rights reserved.
 */

#include <security/attestation/backend_factory.h>

#include <security/attestation/backends/fake/fake_backend.h>
#include <security/attestation/backends/null/null_backend.h>
#include <security/attestation/backends/sgx_dcap/sgx_dcap_backend.h>
#include <security/attestation/backends/sgx_epid/sgx_epid_backend.h>
#include <security/attestation/backends/simulation/simulation_backend.h>

#include <exception>
#include <memory>
#include <utility>

#if defined(CANOPY_PRODUCTION_RELEASE) && defined(CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS)
#  error "Production releases must not build fake or SGX-simulation attestation backend implementations"
#endif

#if !defined(CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS)                                                           \
    && (defined(CANOPY_ATTESTATION_BACKEND_FAKE) || defined(CANOPY_ATTESTATION_BACKEND_SGX_SIM))
#  error "Fake or SGX-simulation attestation backend selected while development backends are disabled"
#endif

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
        case configured_backend_kind::sgx_dcap_backend:
            return sgx_dcap_backend_id;
        }
        std::terminate();
    }

    auto configured_attestation_backend_kind() noexcept -> configured_backend_kind
    {
#ifdef CANOPY_ATTESTATION_BACKEND_NULL
        return configured_backend_kind::null_backend;
#elif defined(CANOPY_ATTESTATION_BACKEND_FAKE)
        return configured_backend_kind::fake_backend;
#elif defined(CANOPY_ATTESTATION_BACKEND_SGX_SIM)
        return configured_backend_kind::sgx_sim_backend;
#elif defined(CANOPY_ATTESTATION_BACKEND_SGX_EPID)
        return configured_backend_kind::sgx_epid_backend;
#elif defined(CANOPY_ATTESTATION_BACKEND_DCAP)
        return configured_backend_kind::sgx_dcap_backend;
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
#ifdef CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS
            return std::make_shared<fake_backend>();
#else
            std::terminate();
#endif
        case configured_backend_kind::sgx_sim_backend:
#ifdef CANOPY_ENABLE_DEVELOPMENT_ATTESTATION_BACKENDS
            return std::make_shared<simulation_backend>();
#else
            std::terminate();
#endif
        case configured_backend_kind::sgx_epid_backend:
            return std::make_shared<sgx_epid_backend>();
        case configured_backend_kind::sgx_dcap_backend:
            return std::make_shared<sgx_dcap_backend>();
        }
        std::terminate();
    }

    auto make_configured_attestation_backend() -> std::shared_ptr<attestation_backend>
    {
        return make_configured_attestation_backend({});
    }

    auto make_configured_attestation_backend(backend_factory_overrides overrides) -> std::shared_ptr<attestation_backend>
    {
        if (overrides.backend)
            return std::move(overrides.backend);
        return make_attestation_backend(configured_attestation_backend_kind());
    }

    auto make_configured_attestation_service_options(
        identity local_identity,
        backend_factory_overrides overrides) -> attestation_service_options
    {
        auto kind = configured_attestation_backend_kind();

        attestation_service_options options;
        options.local_identity = std::move(local_identity);
        if (overrides.backend)
            options.backend = std::move(overrides.backend);
        else
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
        case configured_backend_kind::sgx_dcap_backend:
            options.policy.send_local_evidence = true;
            options.policy.require_peer_evidence = true;
            options.policy.allow_unattested_peer = false;
            options.policy.allow_development_evidence = false;
            options.policy.minimum_security_level = security_level::hardware;
            break;
        }
        return options;
    }
} // namespace canopy::security::attestation
